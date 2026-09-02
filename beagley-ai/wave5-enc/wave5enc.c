// wave5enc — minimal V4L2 mem2mem stateful-encoder harness for the AM67A/J722S
// Wave5 VPU. Feeds raw UYVY frames from a file into the hardware H.264 encoder
// and writes the Annex-B bitstream out, timing the encode loop.
//
// This exists because the minimal image's stock tools can't drive the encoder:
// v4l2-ctl mis-sequences the stateful mplane M2M device (one STREAMON, never
// dequeues), and GStreamer has no encoder plugins. It is also the reference
// for the in-app encoder (dashcam M1) — the exact ioctl sequence the C++
// CameraFeed record path will use. See ../../DASHCAM.md and README.md.
//
// Build (static aarch64, runs on the board regardless of its glibc):
//   gcc -O2 -static wave5enc.c -o wave5enc
// Run on board:
//   ./wave5enc <in.uyvy> <W> <H> <out.h264> [nframes] [bitrate_bps] [gop]
//
// Deliberately single-stream: measure 4-camera load by launching four in
// parallel and timing the batch (the encoder supports 32 instances).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NBUF 8
#define MAXPLANES 1   // UYVY packed and H.264 are both single-plane

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void die(const char *msg) { fprintf(stderr, "wave5enc: %s: %s\n", msg, strerror(errno)); exit(1); }

// Find the encoder node by QUERYCAP driver name, not a fixed /dev/videoN.
static int open_encoder(char *path_out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char p[32]; snprintf(p, sizeof p, "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            strcmp((char *)cap.driver, "wave5-enc") == 0) {
            snprintf(path_out, n, "%s", p);
            return fd;
        }
        close(fd);
    }
    return -1;
}

struct buf { void *start[MAXPLANES]; size_t len[MAXPLANES]; };

static void set_ctrl(int fd, uint32_t id, int32_t val) {
    struct v4l2_ext_control c; memset(&c, 0, sizeof c);
    c.id = id; c.value = val;
    struct v4l2_ext_controls cs; memset(&cs, 0, sizeof cs);
    cs.which = V4L2_CTRL_WHICH_CUR_VAL; cs.count = 1; cs.controls = &c;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) < 0)
        fprintf(stderr, "wave5enc: warn: S_EXT_CTRLS id=0x%x: %s\n", id, strerror(errno));
}

static void set_fmt(int fd, int type, int w, int h, uint32_t fourcc) {
    struct v4l2_format f; memset(&f, 0, sizeof f);
    f.type = type;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    // Match what the cameras emit (BT.601 limited range) so the encoder
    // doesn't stall on colorimetry negotiation.
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_SMPTE170M;
    f.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
    f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    f.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) die("S_FMT");
}

static struct buf *req_and_map(int fd, int type, int count, int *got) {
    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
    rb.count = count; rb.type = type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) die("REQBUFS");
    *got = rb.count;
    struct buf *b = calloc(rb.count, sizeof *b);
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_plane planes[MAXPLANES]; memset(planes, 0, sizeof planes);
        struct v4l2_buffer v; memset(&v, 0, sizeof v);
        v.type = type; v.memory = V4L2_MEMORY_MMAP; v.index = i;
        v.length = MAXPLANES; v.m.planes = planes;
        if (ioctl(fd, VIDIOC_QUERYBUF, &v) < 0) die("QUERYBUF");
        b[i].len[0] = planes[0].length;
        b[i].start[0] = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, planes[0].m.mem_offset);
        if (b[i].start[0] == MAP_FAILED) die("mmap");
    }
    return b;
}

static int qbuf(int fd, int type, int index, size_t bytesused) {
    struct v4l2_plane planes[MAXPLANES]; memset(planes, 0, sizeof planes);
    planes[0].bytesused = bytesused;
    planes[0].length = 0; // ignored on QBUF for MMAP
    struct v4l2_buffer v; memset(&v, 0, sizeof v);
    v.type = type; v.memory = V4L2_MEMORY_MMAP; v.index = index;
    v.length = MAXPLANES; v.m.planes = planes;
    return ioctl(fd, VIDIOC_QBUF, &v);
}

// Returns buffer index >=0, -1 on EAGAIN, -2 on LAST-flag drain end.
static int dqbuf(int fd, int type, size_t *bytesused, int *last) {
    struct v4l2_plane planes[MAXPLANES]; memset(planes, 0, sizeof planes);
    struct v4l2_buffer v; memset(&v, 0, sizeof v);
    v.type = type; v.memory = V4L2_MEMORY_MMAP;
    v.length = MAXPLANES; v.m.planes = planes;
    if (ioctl(fd, VIDIOC_DQBUF, &v) < 0) {
        if (errno == EAGAIN) return -1;
        if (errno == EPIPE) return -2;
        die("DQBUF");
    }
    if (bytesused) *bytesused = planes[0].bytesused;
    if (last) *last = (v.flags & V4L2_BUF_FLAG_LAST) ? 1 : 0;
    return v.index;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <in.uyvy> <W> <H> <out.h264> [nframes] [bitrate_bps] [gop]\n", argv[0]);
        return 2;
    }
    const char *inpath = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]);
    const char *outpath = argv[4];
    int nframes = argc > 5 ? atoi(argv[5]) : 0;      // 0 = all frames in file
    int bitrate = argc > 6 ? atoi(argv[6]) : 8000000;
    int gop = argc > 7 ? atoi(argv[7]) : 25;

    size_t in_frame = (size_t)W * H * 2;             // UYVY = 2 bytes/pixel
    FILE *in = fopen(inpath, "rb");
    if (!in) die("open input");
    fseek(in, 0, SEEK_END); long fsz = ftell(in); fseek(in, 0, SEEK_SET);
    int file_frames = (int)(fsz / in_frame);
    if (file_frames < 1) { fprintf(stderr, "wave5enc: input has no whole frames\n"); return 1; }
    if (nframes <= 0) nframes = file_frames;

    char node[32];
    int fd = open_encoder(node, sizeof node);
    if (fd < 0) { fprintf(stderr, "wave5enc: no wave5-enc node found\n"); return 1; }
    fprintf(stderr, "wave5enc: encoder %s, %dx%d, %d frames, %d bps, gop %d\n",
            node, W, H, nframes, bitrate, gop);

    set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, W, H, V4L2_PIX_FMT_UYVY);
    set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, W, H, V4L2_PIX_FMT_H264);
    set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    set_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop);
    set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, gop);
    // Level 1.0 (the driver default) is non-conformant for 1080p; set 4.2. The
    // encoder writes this into the SPS — not the cause of any hang, just correct.
    set_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2);

    int nout, ncap;
    struct buf *ob = req_and_map(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NBUF, &nout);
    struct buf *cb = req_and_map(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, NBUF, &ncap);

    FILE *out = fopen(outpath, "wb");
    if (!out) die("open output");

    // Prime: queue all capture buffers, and as many output frames as buffers.
    for (int i = 0; i < ncap; i++)
        if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 0) < 0) die("QBUF cap");

    int fed = 0;
    for (int i = 0; i < nout && fed < nframes; i++) {
        if (fread(ob[i].start[0], 1, in_frame, in) != in_frame) { fseek(in, 0, SEEK_SET); fread(ob[i].start[0], 1, in_frame, in); }
        if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i, ob[i].len[0]) < 0) die("QBUF out");
        fed++;
    }

    // ORDER IS LOad-BEARING: capture queue must be streaming before the output
    // queue's start_streaming runs, or the driver never reaches PIC_RUN and the
    // encoder silently produces nothing. vb2 sets q->streaming only after the
    // callback returns, so STREAMON(capture) must precede STREAMON(output).
    // (wave5_vpu_enc_start_streaming: `state == OPEN && cap_q.streaming` gate.)
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) die("STREAMON cap");
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) die("STREAMON out");

    double t0 = now_s();
    int encoded = 0, stopped = 0;
    long outbytes = 0;
    while (1) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
        if (poll(&pfd, 1, 2000) <= 0) { fprintf(stderr, "wave5enc: poll timeout (encoded=%d)\n", encoded); break; }

        // Reclaim consumed input buffers; refill or signal EOS.
        if (pfd.revents & POLLOUT) {
            int idx = dqbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NULL, NULL);
            if (idx >= 0) {
                if (fed < nframes) {
                    if (fread(ob[idx].start[0], 1, in_frame, in) != in_frame) { fseek(in, 0, SEEK_SET); fread(ob[idx].start[0], 1, in_frame, in); }
                    if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, idx, ob[idx].len[0]) < 0) die("QBUF out");
                    fed++;
                } else if (!stopped) {
                    struct v4l2_encoder_cmd cmd; memset(&cmd, 0, sizeof cmd);
                    cmd.cmd = V4L2_ENC_CMD_STOP;
                    ioctl(fd, VIDIOC_ENCODER_CMD, &cmd);   // best-effort drain
                    stopped = 1;
                }
            }
        }

        // Drain coded frames.
        if (pfd.revents & (POLLIN | POLLERR)) {
            size_t used; int last;
            int idx = dqbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &used, &last);
            if (idx == -2) break;                 // EPIPE = end of drain
            if (idx >= 0) {
                if (used > 0) { fwrite(cb[idx].start[0], 1, used, out); outbytes += used; encoded++; }
                if (last) break;
                if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, idx, 0) < 0) die("QBUF cap");
            }
        }
    }
    double dt = now_s() - t0;

    // Clean teardown. Stopping both queues before close matters: an unclean
    // exit while streaming can leave the VPU firmware mid-command and wedge it
    // (observed — a release then blocks >120s in-firmware and leaks the
    // instance, requiring a cold power-cycle to recover).
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  ioctl(fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(fd, VIDIOC_STREAMOFF, &t);
    close(fd);
    fclose(out);
    fprintf(stderr, "wave5enc: RESULT %d frames in %.3fs = %.1f fps, %ld bytes (~%.2f Mbit/s @%dfps)\n",
            encoded, dt, dt > 0 ? encoded / dt : 0, outbytes,
            encoded > 0 ? outbytes * 8.0 / (encoded / 25.0) / 1e6 : 0, 25);
    printf("%.1f\n", dt > 0 ? encoded / dt : 0);   // stdout: fps only, for scripting
    return 0;
}
