/*
 * ultima-readahead — record which pages of which files a running process
 * actually has in the page cache, and replay that as readahead(2) calls at
 * the next boot. The BeagleY-AI dash's libraries come off an SD card; a
 * whole-file `cat` prefetch (ultima-prefetch.list) reads ~60 MB when the app
 * touches ~40, and reads it in list order rather than the order the dynamic
 * linker wants it. A recorded pack fixes both.
 *
 *   ultima-readahead record <pack> <pid> [extra-file ...]
 *       Every file mapped by <pid> (/proc/<pid>/maps, first-seen order) plus
 *       the extras is mmap'd and mincore()'d; runs of resident pages, with
 *       gaps of up to 64 KiB bridged to keep the I/O sequential, are written
 *       as "path size offset length" lines. Record on a boot where the
 *       whole-file prefetch did NOT run (mask ultima-prefetch.service),
 *       right after the first frame, so the cache holds what the boot needed
 *       and not what a prefetch dumped into it.
 *
 *   ultima-readahead replay <pack>
 *       readahead(2) every range, in order. A file whose size no longer
 *       matches the recorded one (stale pack after an update) is read whole
 *       instead, capped at 32 MiB; a missing file is skipped. Never fails
 *       the boot.
 *
 * No libc beyond POSIX; no threads; ~40 MB of ranges replay in well under a
 * second on the board.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILES 512
#define MERGE_GAP_PAGES 16      /* bridge gaps of <= 64 KiB (4 KiB pages) */
#define WHOLE_FILE_LIMIT (32L * 1024 * 1024)   /* size-mismatch fallback cap */

static const char *files[MAX_FILES];
static int nfiles;

static void add_file(const char *path)
{
    if (path[0] != '/' || strstr(path, "(deleted)") || nfiles >= MAX_FILES)
        return;
    if (!strncmp(path, "/dev/", 5) || !strncmp(path, "/proc/", 6) ||
        !strncmp(path, "/sys/", 5) || !strncmp(path, "/run/", 5))
        return;
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i], path))
            return;
    files[nfiles++] = strdup(path);
}

static int record_file(FILE *out, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        close(fd);
        return -1;
    }
    long pg = sysconf(_SC_PAGESIZE);
    size_t npages = (size_t)((st.st_size + pg - 1) / pg);
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    unsigned char *vec = malloc(npages);
    if (!vec || mincore(map, (size_t)st.st_size, vec) < 0) {
        free(vec);
        munmap(map, (size_t)st.st_size);
        close(fd);
        return -1;
    }
    /* Emit runs of resident pages, bridging short gaps. */
    long run_start = -1, last_res = -1;
    for (size_t i = 0; i <= npages; i++) {
        int res = i < npages && (vec[i] & 1);
        if (res) {
            if (run_start < 0)
                run_start = (long)i;
            else if ((long)i - last_res - 1 > MERGE_GAP_PAGES) {
                /* gap too big: flush the previous run */
                long off = run_start * pg, len = (last_res + 1) * pg - off;
                if (off + len > st.st_size) len = st.st_size - off;
                fprintf(out, "%s %lld %ld %ld\n", path, (long long)st.st_size, off, len);
                run_start = (long)i;
            }
            last_res = (long)i;
        }
        if (i == npages && run_start >= 0) {
            long off = run_start * pg, len = (last_res + 1) * pg - off;
            if (off + len > st.st_size) len = st.st_size - off;
            fprintf(out, "%s %lld %ld %ld\n", path, (long long)st.st_size, off, len);
        }
    }
    free(vec);
    munmap(map, (size_t)st.st_size);
    close(fd);
    return 0;
}

static int do_record(const char *pack, const char *pid, int nextra, char **extra)
{
    char mapspath[64];
    snprintf(mapspath, sizeof mapspath, "/proc/%s/maps", pid);
    FILE *maps = fopen(mapspath, "r");
    if (!maps) {
        fprintf(stderr, "ultima-readahead: %s: %s\n", mapspath, strerror(errno));
        return 1;
    }
    /* The executable first: it's what the kernel reads before anything else. */
    char exe[PATH_MAX];
    snprintf(mapspath, sizeof mapspath, "/proc/%s/exe", pid);
    ssize_t n = readlink(mapspath, exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        add_file(exe);
    }
    char line[PATH_MAX + 128];
    while (fgets(line, sizeof line, maps)) {
        char *p = strchr(line, '/');
        if (!p)
            continue;
        p[strcspn(p, "\n")] = '\0';
        add_file(p);
    }
    fclose(maps);
    for (int i = 0; i < nextra; i++)
        add_file(extra[i]);

    FILE *out = fopen(pack, "w");
    if (!out) {
        fprintf(stderr, "ultima-readahead: %s: %s\n", pack, strerror(errno));
        return 1;
    }
    fprintf(out, "# ultima-readahead pack: path size offset length (bytes)\n");
    long long total = 0;
    for (int i = 0; i < nfiles; i++)
        record_file(out, files[i]);
    fclose(out);
    /* Report the total for the log. */
    out = fopen(pack, "r");
    if (out) {
        while (fgets(line, sizeof line, out)) {
            long long sz, off, len;
            char path[PATH_MAX];
            if (sscanf(line, "%s %lld %lld %lld", path, &sz, &off, &len) == 4)
                total += len;
        }
        fclose(out);
    }
    fprintf(stderr, "ultima-readahead: recorded %d files, %lld KiB of ranges to %s\n",
            nfiles, total / 1024, pack);
    return 0;
}

static int do_replay(const char *pack)
{
    FILE *in = fopen(pack, "r");
    if (!in)
        return 0;               /* no pack, nothing to do — never fail the boot */
    char line[PATH_MAX + 128], path[PATH_MAX], cur[PATH_MAX] = "";
    int fd = -1;
    long long cur_size = -1;
    while (fgets(line, sizeof line, in)) {
        long long sz, off, len;
        if (line[0] == '#' || sscanf(line, "%s %lld %lld %lld", path, &sz, &off, &len) != 4)
            continue;
        if (strcmp(path, cur)) {
            if (fd >= 0)
                close(fd);
            strcpy(cur, path);
            cur_size = -1;
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0)
                    cur_size = st.st_size;
                /* Changed since the recording (a rebuilt app or library):
                 * the ranges no longer mean anything, but the file is still
                 * about to be mapped, so read all of it — once, bounded —
                 * rather than nothing. Re-record the pack to get the exact
                 * ranges back. */
                if (cur_size >= 0 && cur_size != sz) {
                    readahead(fd, 0, (size_t)(cur_size < WHOLE_FILE_LIMIT ? cur_size : WHOLE_FILE_LIMIT));
                    close(fd);
                    fd = -1;
                }
            }
        }
        if (fd < 0 || cur_size != sz)
            continue;           /* missing, or already handled above */
        readahead(fd, off, (size_t)len);
    }
    if (fd >= 0)
        close(fd);
    fclose(in);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "record"))
        return do_record(argv[2], argv[3], argc - 4, argv + 4);
    if (argc == 3 && !strcmp(argv[1], "replay"))
        return do_replay(argv[2]);
    fprintf(stderr, "usage: ultima-readahead record <pack> <pid> [extra-file ...]\n"
                    "       ultima-readahead replay <pack>\n");
    return 2;
}
