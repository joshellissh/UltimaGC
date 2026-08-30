/*
 * Blits a pre-converted raw image straight into /dev/fb0 and exits.
 *
 * Why /dev/fb0 instead of a real DRM/KMS client, and why this never opens
 * /dev/dri/card0: see GAUGE-CLUSTER.md "Boot splash" -- this is the same
 * proven pixel path, now blitting real artwork instead of a procedural
 * test pattern.
 *
 * The image ships as a headerless raw blob (SPLASH_IMAGE_PATH), not the
 * source PNG -- converted at build/edit time via PIL
 * (Image.convert("RGB").tobytes("raw", "BGRX")), which on this
 * little-endian target produces exactly the in-memory byte order XRGB8888
 * wants (B,G,R,pad per pixel, so a 32-bit native read is 0x00RRGGBB -- the
 * same layout ultima-splash.c already confirmed against this panel's
 * fb_var_screeninfo). That keeps the target-side code to a plain read()
 * loop -- no PNG/zlib decoding on the board, nothing new for the
 * read-only rootfs to carry beyond the one flat file. Regenerate the blob
 * with:
 *   python3 -c 'from PIL import Image; \
 *     Image.open("splash screen.png").convert("RGB") \
 *       .tobytes("raw", "BGRX")' > splash.rgbx
 * (see files/splash.rgbx's own provenance in the recipe).
 *
 * Deliberately refuses to draw anything if the file size doesn't match
 * the panel's current resolution exactly, rather than guessing how to
 * scale or crop -- same "refuse to guess" stance as the bpp check below.
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SPLASH_IMAGE_PATH "/usr/share/ultima-splash/splash.rgbx"

int main(void) {
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        perror("ultima-splash: open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ultima-splash: FBIOGET_*SCREENINFO");
        close(fbfd);
        return 1;
    }

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "ultima-splash: expected 32bpp, got %u -- refusing to guess a layout\n",
                vinfo.bits_per_pixel);
        close(fbfd);
        return 1;
    }

    int imgfd = open(SPLASH_IMAGE_PATH, O_RDONLY);
    if (imgfd < 0) {
        perror("ultima-splash: open " SPLASH_IMAGE_PATH);
        close(fbfd);
        return 1;
    }

    struct stat st;
    if (fstat(imgfd, &st) < 0) {
        perror("ultima-splash: fstat splash image");
        close(imgfd);
        close(fbfd);
        return 1;
    }

    size_t expected = (size_t)vinfo.xres * vinfo.yres * 4;
    if ((size_t)st.st_size != expected) {
        fprintf(stderr,
                "ultima-splash: splash image is %lld bytes, panel is %ux%u (expected %zu) "
                "-- refusing to guess how to scale or crop\n",
                (long long)st.st_size, vinfo.xres, vinfo.yres, expected);
        close(imgfd);
        close(fbfd);
        return 1;
    }

    size_t screensize = (size_t)finfo.line_length * vinfo.yres_virtual;
    uint8_t *fb = mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) {
        perror("ultima-splash: mmap");
        close(imgfd);
        close(fbfd);
        return 1;
    }

    size_t row_bytes = (size_t)vinfo.xres * 4;
    for (uint32_t y = 0; y < vinfo.yres; y++) {
        uint8_t *row = fb + (size_t)y * finfo.line_length;
        size_t got = 0;
        while (got < row_bytes) {
            ssize_t n = read(imgfd, row + got, row_bytes - got);
            if (n < 0) {
                perror("ultima-splash: read splash image");
                munmap(fb, screensize);
                close(imgfd);
                close(fbfd);
                return 1;
            }
            if (n == 0) {
                fprintf(stderr, "ultima-splash: splash image ended early at row %u\n", y);
                munmap(fb, screensize);
                close(imgfd);
                close(fbfd);
                return 1;
            }
            got += (size_t)n;
        }
    }

    msync(fb, screensize, MS_SYNC);
    munmap(fb, screensize);
    close(imgfd);
    close(fbfd);
    return 0;
}
