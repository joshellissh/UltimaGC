// camqa.c — standalone V4L2 + DRM/KMS camera QA tool for BeagleY-AI (AM67A/J722S)
//
// Purpose
//   Manual bring-up / bench test for the NVP6324 (MY-CAM004M) 4-channel AHD ->
//   MIPI-CSI2 camera path. The kernel CSI2RX stack demuxes the 4 virtual
//   channels into four independent V4L2 capture nodes /dev/video0..3, each
//   streaming UYVY 1920x1080 nominally at 25 fps. This tool captures all four
//   and paints them as a fullscreen 2x2 grid on the tidss DRM display via a
//   single dumb framebuffer, with an explicit per-channel "frames arriving?"
//   readout — because the driver forces 1080p25, a camera set to a different
//   standard (or an unplugged input) produces a black quadrant with no other
//   symptom, and the whole point of this tool is to make that visible.
//
// Dependencies: libc + V4L2 (linux/videodev2.h) + libdrm only.
//   No Qt, no GStreamer, no X11, no Wayland.
//
// It must be run while nothing else owns the display: stop ultima-app.service
// first (see README). --probe mode is headless (no DRM) and is safe to run on
// a live board without stopping anything.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/videodev2.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

// ---------------------------------------------------------------------------
// Constants / config
// ---------------------------------------------------------------------------
#define MAX_CAM      8      // hard cap on capture nodes
#define MAX_BUF      8      // hard cap on V4L2 buffers per node
#define REQ_BUFS     4      // buffers we ask the driver for
#define DEF_WIDTH    1920
#define DEF_HEIGHT   1080
#define DEF_COUNT    4
#define POLL_TIMEOUT_MS 200
#define TICK_MS      1000   // status readout / no-signal cadence
#define PROBE_MS     3000   // how long --probe streams before reporting

// ---------------------------------------------------------------------------
// Global stop flag, set from a signal handler (no SA_RESTART, so poll() and
// blocking ioctls return EINTR and the main loop notices).
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// Diagnostics: per-frame bytesused logging + one-shot raw buffer dump. Set from
// CLI. g_verbose prints every dequeued frame's bytesused; g_dump_path, when set,
// writes the first settled full-size buffer to that file (from inside the live
// stream, so the raw bytes match exactly what is on screen).
static int         g_verbose   = 0;
static const char *g_dump_path = NULL;
static int         g_dump_done = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ioctl wrapper that retries on EINTR only.
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// ---------------------------------------------------------------------------
// Per-camera state
// ---------------------------------------------------------------------------
struct cam {
    int      node;                 // N in /dev/videoN
    char     path[32];
    int      fd;                   // -1 if not opened
    int      opened;               // QUERYCAP/S_FMT/streamon all succeeded
    int      streaming;

    // negotiated format (read back from the driver, not assumed)
    uint32_t width, height;
    uint32_t pixfmt;
    uint32_t bytesperline;
    uint32_t sizeimage;

    // mmap'd buffers
    struct { void *start; size_t length; } buf[MAX_BUF];
    unsigned n_buffers;

    // live stats
    unsigned long frames_total;    // since start
    unsigned      frames_window;   // since last status tick
    double        fps;             // computed at last tick
    int           live;            // frames_window > 0 at last tick

    // bytesused instrumentation (how much of sizeimage the driver actually
    // filled per frame — the discriminating measurement for the short-frame /
    // 2/3-frame / shear bug). min/max are cumulative over the whole run.
    uint32_t      bu_last;         // most recent buffer's bytesused
    uint32_t      bu_min, bu_max;  // cumulative extremes (bu_min==0 => unset)
    unsigned long bu_partial;      // count of frames with bytesused != sizeimage
};

// ---------------------------------------------------------------------------
// DRM display state
// ---------------------------------------------------------------------------
struct drm_dev {
    int              fd;
    uint32_t         conn_id;
    uint32_t         crtc_id;
    drmModeModeInfo  mode;         // chosen mode (fb is sized to this)
    drmModeCrtc     *saved_crtc;   // to attempt-restore on teardown

    uint32_t         fb_id;
    uint32_t         handle;       // dumb bo handle
    uint32_t         pitch;        // bytes per row of the FB (NOT width*4)
    uint64_t         size;
    uint8_t         *map;          // mmap of the dumb bo
};

// ---------------------------------------------------------------------------
// Grid layout: quadrant geometry + scaling lookup tables (shared, since every
// quadrant is the same size). slot i -> cam[i].
// ---------------------------------------------------------------------------
struct layout {
    int  n;
    int  cx[MAX_CAM], cy[MAX_CAM];  // cell top-left in the FB
    int  cw, ch;                    // cell size (all equal)
    // Aspect-preserved image rectangle inside each cell (letterbox/pillarbox).
    // Same size for every cell (identical source dims); only the origin differs.
    int  px[MAX_CAM], py[MAX_CAM];  // image top-left in the FB
    int  pw, ph;                    // image size, source aspect preserved
    int *sx_lut;                    // pw entries: dst col -> src col
    int *sy_lut;                    // ph entries: dst row -> src row
    int  lut_srcw, lut_srch;        // src dims the LUTs were built for
};

// ---------------------------------------------------------------------------
// 5x7 bitmap font — just what we need: digits 0-9, N O S I G, space.
// Each glyph is 7 rows; bit4..bit0 = left..right.
// ---------------------------------------------------------------------------
static const uint8_t FONT_DIGIT[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
};
static const uint8_t GLYPH_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
static const uint8_t GLYPH_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t GLYPH_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
static const uint8_t GLYPH_I[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F};
static const uint8_t GLYPH_G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
static const uint8_t GLYPH_SP[7] = {0,0,0,0,0,0,0};

static const uint8_t *glyph_for(char c)
{
    if (c >= '0' && c <= '9') return FONT_DIGIT[c - '0'];
    switch (c) {
        case 'N': return GLYPH_N;
        case 'O': return GLYPH_O;
        case 'S': return GLYPH_S;
        case 'I': return GLYPH_I;
        case 'G': return GLYPH_G;
        default:  return GLYPH_SP;
    }
}

// ===========================================================================
// Framebuffer drawing primitives (operate on the mapped dumb bo)
// ===========================================================================

// Fill a rectangle, clipped to the FB. color is 0x00RRGGBB (XRGB8888).
static void fb_fill(struct drm_dev *d, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)d->mode.hdisplay) w = (int)d->mode.hdisplay - x;
    if (y + h > (int)d->mode.vdisplay) h = (int)d->mode.vdisplay - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(d->map + (size_t)(y + j) * d->pitch) + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

// Draw a hollow border rectangle of the given thickness.
static void fb_border(struct drm_dev *d, int x, int y, int w, int h, int t, uint32_t color)
{
    fb_fill(d, x, y, w, t, color);             // top
    fb_fill(d, x, y + h - t, w, t, color);     // bottom
    fb_fill(d, x, y, t, h, color);             // left
    fb_fill(d, x + w - t, y, t, h, color);     // right
}

// Draw one glyph scaled by 'scale'. Only set bits are painted (transparent bg).
static void fb_glyph(struct drm_dev *d, int x, int y, int scale,
                     const uint8_t *g, uint32_t color)
{
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (g[row] & (1 << (4 - col)))
                fb_fill(d, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

// Draw a string; advances 6*scale px per char (5 wide + 1 gap).
static void fb_text(struct drm_dev *d, int x, int y, int scale,
                    const char *s, uint32_t color)
{
    for (; *s; s++) {
        fb_glyph(d, x, y, scale, glyph_for(*s), color);
        x += 6 * scale;
    }
}

// ===========================================================================
// UYVY -> XRGB8888, nearest-neighbour scaled into a quadrant.
//
// UYVY memory order is: U0 Y0 V0 Y1  (one 4-byte macropixel = 2 pixels).
// So for source column sx: the macropixel base is (sx>>1)*4 bytes into the row,
// U = base[0], V = base[2], and Y = base[1] for the even pixel / base[3] for
// the odd pixel. BT.601 limited-range integer coefficients (these AHD cameras
// emit limited-range YUV; getting this wrong is silently-wrong colors).
// ===========================================================================
static void uyvy_to_xrgb_scaled(struct drm_dev *d, const uint8_t *src,
                                int src_bpl, int dx, int dy,
                                const struct layout *L)
{
    const int dw = L->pw, dh = L->ph;
    for (int j = 0; j < dh; j++) {
        const uint8_t *srow = src + (size_t)L->sy_lut[j] * src_bpl;
        uint32_t *drow = (uint32_t *)(d->map + (size_t)(dy + j) * d->pitch) + dx;
        for (int i = 0; i < dw; i++) {
            int sx = L->sx_lut[i];
            const uint8_t *mp = srow + (size_t)(sx >> 1) * 4;
            int u = mp[0];
            int v = mp[2];
            int y = (sx & 1) ? mp[3] : mp[1];
            int c = y - 16, dd = u - 128, ee = v - 128;
            int r = (298 * c + 409 * ee + 128) >> 8;
            int g = (298 * c - 100 * dd - 208 * ee + 128) >> 8;
            int b = (298 * c + 516 * dd + 128) >> 8;
            drow[i] = ((uint32_t)clamp8(r) << 16) |
                      ((uint32_t)clamp8(g) << 8)  |
                       (uint32_t)clamp8(b);
        }
    }
}

// Draw the per-quadrant status overlay: colored border (green=receiving,
// red=no frames in the last second) + a label showing the node index and,
// when dead, "NO SIG".
static void overlay_status(struct drm_dev *d, const struct layout *L, int slot,
                           int node, int live)
{
    // Frame the actual image rectangle (not the whole cell), so the border hugs
    // the picture and the letterbox bars stay clean black.
    int x = L->px[slot], y = L->py[slot];
    uint32_t col = live ? 0x0000C000u : 0x00E00000u; // green : red
    int t = L->ph / 120; if (t < 3) t = 3;
    fb_border(d, x, y, L->pw, L->ph, t, col);

    int scale = L->ph / 120; if (scale < 2) scale = 2; if (scale > 6) scale = 6;
    char label[32];
    if (live) snprintf(label, sizeof label, "%d", node);
    else      snprintf(label, sizeof label, "%d NO SIG", node);

    int pad = 4 * scale;
    int tx = x + t + pad, ty = y + t + pad;
    int tw = (int)strlen(label) * 6 * scale + 2 * pad;
    int th = 7 * scale + 2 * pad;
    fb_fill(d, tx - pad, ty - pad, tw, th, 0x00000000u); // dark backing box
    fb_text(d, tx, ty, scale, label, 0x00FFFFFFu);
}

// ===========================================================================
// V4L2 capture
// ===========================================================================

// Queue a buffer back to the driver.
static int cam_qbuf(struct cam *c, unsigned index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;
    if (xioctl(c->fd, VIDIOC_QBUF, &buf) == -1) {
        fprintf(stderr, "%s: VIDIOC_QBUF(%u): %s\n", c->path, index, strerror(errno));
        return -1;
    }
    return 0;
}

// Open one node and bring it all the way to STREAMON. On any failure the node
// is left marked !opened (fd closed) so a dead/absent node never blocks the
// others — the caller keeps going with whatever opened.
static int cam_open(struct cam *c, uint32_t want_w, uint32_t want_h)
{
    c->fd = open(c->path, O_RDWR | O_NONBLOCK); // NONBLOCK so DQBUF can EAGAIN
    if (c->fd == -1) {
        fprintf(stderr, "%s: open: %s\n", c->path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(c->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "%s: VIDIOC_QUERYCAP: %s\n", c->path, strerror(errno));
        goto fail;
    }
    // Prefer device_caps (per-node) when the driver advertises it.
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s: no VIDEO_CAPTURE capability (0x%08x)\n", c->path, caps);
        goto fail;
    }
    if (!(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s: no STREAMING capability (0x%08x)\n", c->path, caps);
        goto fail;
    }

    // S_FMT: UYVY, requested WxH, progressive.
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = want_w;
    fmt.fmt.pix.height      = want_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) == -1) {
        fprintf(stderr, "%s: VIDIOC_S_FMT: %s\n", c->path, strerror(errno));
        goto fail;
    }
    // Honor whatever the driver actually gave us.
    c->width        = fmt.fmt.pix.width;
    c->height       = fmt.fmt.pix.height;
    c->pixfmt       = fmt.fmt.pix.pixelformat;
    c->bytesperline = fmt.fmt.pix.bytesperline;
    c->sizeimage    = fmt.fmt.pix.sizeimage;
    if (c->bytesperline == 0) c->bytesperline = c->width * 2; // paranoia
    if (c->pixfmt != V4L2_PIX_FMT_UYVY)
        fprintf(stderr, "%s: warning: driver chose fourcc %.4s, not UYVY\n",
                c->path, (char *)&c->pixfmt);

    // REQBUFS — the driver may grant fewer than we asked; honor its count.
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof req);
    req.count  = REQ_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) == -1) {
        fprintf(stderr, "%s: VIDIOC_REQBUFS: %s\n", c->path, strerror(errno));
        goto fail;
    }
    if (req.count < 2) {
        fprintf(stderr, "%s: only %u buffers granted, need >=2\n", c->path, req.count);
        goto fail;
    }

    // QUERYBUF + mmap each.
    c->n_buffers = 0;
    for (unsigned i = 0; i < req.count && i < MAX_BUF; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            fprintf(stderr, "%s: VIDIOC_QUERYBUF(%u): %s\n", c->path, i, strerror(errno));
            goto fail_unmap;
        }
        c->buf[i].length = buf.length;
        c->buf[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, c->fd, buf.m.offset);
        if (c->buf[i].start == MAP_FAILED) {
            fprintf(stderr, "%s: mmap(buf %u): %s\n", c->path, i, strerror(errno));
            c->buf[i].start = NULL;
            goto fail_unmap;
        }
        c->n_buffers++;
    }

    // Queue all, then STREAMON.
    for (unsigned i = 0; i < c->n_buffers; i++)
        if (cam_qbuf(c, i) == -1) goto fail_unmap;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &type) == -1) {
        fprintf(stderr, "%s: VIDIOC_STREAMON: %s\n", c->path, strerror(errno));
        goto fail_unmap;
    }

    c->opened    = 1;
    c->streaming = 1;
    return 0;

fail_unmap:
    for (unsigned i = 0; i < c->n_buffers; i++)
        if (c->buf[i].start) munmap(c->buf[i].start, c->buf[i].length);
    c->n_buffers = 0;
fail:
    close(c->fd);
    c->fd = -1;
    c->opened = 0;
    return -1;
}

static void cam_close(struct cam *c)
{
    if (c->fd < 0) return;
    if (c->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(c->fd, VIDIOC_STREAMOFF, &type);
        c->streaming = 0;
    }
    for (unsigned i = 0; i < c->n_buffers; i++)
        if (c->buf[i].start) munmap(c->buf[i].start, c->buf[i].length);
    c->n_buffers = 0;
    close(c->fd);
    c->fd = -1;
}

// Drain all currently-available frames for one node, count every one, and
// return the index of the newest (or -1 if none / error). Older frames in the
// backlog are requeued immediately — with a CPU blit at 4x1080p25 we can fall
// behind real time, so we always paint the newest frame and keep lag bounded
// instead of accumulating a growing delay. The returned buffer is still owned
// by us (caller must requeue it after using it).
static int cam_drain_newest(struct cam *c)
{
    int newest = -1;
    for (;;) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(c->fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) break;          // nothing (more) available
            fprintf(stderr, "%s: VIDIOC_DQBUF: %s\n", c->path, strerror(errno));
            break;
        }
        c->frames_total++;
        c->frames_window++;
        // Track how full the driver reported this buffer. sizeimage is the full
        // 1080-line frame; a persistent ~2/3 value means a Frame End is landing
        // early (the short-frame bug we're chasing).
        c->bu_last = buf.bytesused;
        if (c->bu_min == 0 || buf.bytesused < c->bu_min) c->bu_min = buf.bytesused;
        if (buf.bytesused > c->bu_max) c->bu_max = buf.bytesused;
        if (buf.bytesused != c->sizeimage) c->bu_partial++;
        if (g_verbose) {
            unsigned bpl = c->bytesperline ? c->bytesperline : 1;
            fprintf(stderr, "%s f%lu bytesused=%u (~%u lines)\n",
                    c->path, c->frames_total, buf.bytesused, buf.bytesused / bpl);
        }
        if (newest >= 0) cam_qbuf(c, newest);    // drop the older backlog frame
        newest = buf.index;
    }
    return newest;
}

// ===========================================================================
// DRM/KMS setup
// ===========================================================================

// Find a suitable CRTC for the given connector. Prefer the encoder/crtc it is
// already wired to; otherwise scan encoders and pick the first CRTC allowed by
// possible_crtcs. Returns crtc_id or 0.
static uint32_t pick_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            uint32_t id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (id) return id;
        }
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) continue;
        for (int j = 0; j < res->count_crtcs; j++) {
            if (enc->possible_crtcs & (1u << j)) {
                uint32_t id = res->crtcs[j];
                drmModeFreeEncoder(enc);
                return id;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return 0;
}

static int drm_init(struct drm_dev *d, const char *path)
{
    memset(d, 0, sizeof *d);
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd == -1) {
        fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
        return -1;
    }
    // Best-effort: become DRM master. Being the sole opener of card0 (after
    // ultima-app is stopped) normally grants this implicitly; ignore failure.
    drmSetMaster(d->fd);

    drmModeRes *res = drmModeGetResources(d->fd);
    if (!res || res->count_connectors == 0) {
        // On this board the PowerVR GPU can enumerate as a second DRM node with
        // no connectors; tidss is card0. Make --device meaningful.
        fprintf(stderr, "%s: not a KMS/display card (no connectors) — try --device /dev/dri/cardX\n", path);
        if (res) drmModeFreeResources(res);
        goto fail;
    }

    // First connected connector with at least one mode.
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(d->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "%s: no connected connector with modes\n", path);
        drmModeFreeResources(res);
        goto fail;
    }
    d->conn_id = conn->connector_id;

    // Preferred mode if flagged, else the first (drmModeGetConnector lists the
    // preferred/highest first).
    int mi = 0;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mi = i; break; }
    d->mode = conn->modes[mi];

    d->crtc_id = pick_crtc(d->fd, res, conn);
    if (!d->crtc_id) {
        fprintf(stderr, "%s: no usable CRTC for connector %u\n", path, d->conn_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        goto fail;
    }
    d->saved_crtc = drmModeGetCrtc(d->fd, d->crtc_id); // for attempt-restore

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    // Create ONE dumb buffer sized to the mode.
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width  = d->mode.hdisplay;
    creq.height = d->mode.vdisplay;
    creq.bpp    = 32;
    if (xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) == -1) {
        fprintf(stderr, "%s: CREATE_DUMB: %s\n", path, strerror(errno));
        goto fail;
    }
    d->handle = creq.handle;
    d->pitch  = creq.pitch;   // use this everywhere, never width*4
    d->size   = creq.size;

    // Add an FB over the dumb bo. Prefer AddFB2 (explicit fourcc); fall back to
    // the legacy AddFB (depth 24 / bpp 32) if the driver rejects it.
    uint32_t handles[4] = { d->handle, 0, 0, 0 };
    uint32_t pitches[4] = { d->pitch,  0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(d->fd, d->mode.hdisplay, d->mode.vdisplay,
                      DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &d->fb_id, 0) != 0) {
        if (drmModeAddFB(d->fd, d->mode.hdisplay, d->mode.vdisplay,
                         24, 32, d->pitch, d->handle, &d->fb_id) != 0) {
            fprintf(stderr, "%s: drmModeAddFB(2): %s\n", path, strerror(errno));
            goto fail;
        }
    }

    // Map it.
    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = d->handle;
    if (xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == -1) {
        fprintf(stderr, "%s: MAP_DUMB: %s\n", path, strerror(errno));
        goto fail;
    }
    d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, mreq.offset);
    if (d->map == MAP_FAILED) {
        fprintf(stderr, "%s: mmap(fb): %s\n", path, strerror(errno));
        d->map = NULL;
        goto fail;
    }
    memset(d->map, 0, d->size); // start black

    // Modeset onto our FB.
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->fb_id, 0, 0,
                       &d->conn_id, 1, &d->mode) != 0) {
        fprintf(stderr, "%s: drmModeSetCrtc: %s\n", path, strerror(errno));
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "  another process owns the display — "
                            "run: systemctl stop ultima-app.service\n");
        goto fail;
    }

    fprintf(stderr, "DRM: %s connector %u, crtc %u, mode %ux%u@%uHz, pitch %u\n",
            path, d->conn_id, d->crtc_id, d->mode.hdisplay, d->mode.vdisplay,
            d->mode.vrefresh, d->pitch);
    return 0;

fail:
    // Partial-init cleanup.
    if (d->map && d->map != MAP_FAILED) munmap(d->map, d->size);
    if (d->fb_id) drmModeRmFB(d->fd, d->fb_id);
    if (d->handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = d->handle;
        xioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    if (d->saved_crtc) drmModeFreeCrtc(d->saved_crtc);
    close(d->fd);
    d->fd = -1;
    return -1;
}

static void drm_cleanup(struct drm_dev *d)
{
    if (d->fd < 0) return;
    // Attempt to restore the previous CRTC config. Note: after ultima-app.service
    // is stopped, the saved CRTC references that process's now-freed FB, so this
    // SetCrtc will usually fail — that's fine, we just leave our last frame on
    // screen (the task explicitly allows restore-or-keep).
    if (d->saved_crtc) {
        drmModeSetCrtc(d->fd, d->saved_crtc->crtc_id, d->saved_crtc->buffer_id,
                       d->saved_crtc->x, d->saved_crtc->y,
                       &d->conn_id, 1, &d->saved_crtc->mode);
        drmModeFreeCrtc(d->saved_crtc);
        d->saved_crtc = NULL;
    }
    if (d->map && d->map != MAP_FAILED) munmap(d->map, d->size);
    if (d->fb_id) drmModeRmFB(d->fd, d->fb_id);
    if (d->handle) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = d->handle;
        xioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    drmDropMaster(d->fd);
    close(d->fd);
    d->fd = -1;
}

// ===========================================================================
// Layout / LUT
// ===========================================================================
static void layout_compute(struct layout *L, int n, int mode_w, int mode_h,
                           int src_w, int src_h)
{
    if (n < 1) n = 1;
    if (n > MAX_CAM) n = MAX_CAM;
    L->n = n;
    int cols = (n <= 1) ? 1 : 2;
    int rows = (n + cols - 1) / cols;
    L->cw = mode_w / cols;
    L->ch = mode_h / rows;

    // Fit the source into a cell while preserving its aspect ratio: the largest
    // src-proportioned rectangle that fits. The uncovered remainder of the cell
    // stays black (letterbox above/below or pillarbox left/right). All cells are
    // identical size and share one source size, so pw/ph are common; only the
    // per-cell centering origin differs.
    if (src_w < 1) src_w = 1;
    if (src_h < 1) src_h = 1;
    if ((long)L->cw * src_h <= (long)L->ch * src_w) {
        L->pw = L->cw;                                   // width-limited
        L->ph = (int)((long)L->cw * src_h / src_w);
    } else {
        L->ph = L->ch;                                   // height-limited
        L->pw = (int)((long)L->ch * src_w / src_h);
    }
    if (L->pw < 1) L->pw = 1;
    if (L->ph < 1) L->ph = 1;

    for (int i = 0; i < n; i++) {
        L->cx[i] = (i % cols) * L->cw;
        L->cy[i] = (i / cols) * L->ch;
        L->px[i] = L->cx[i] + (L->cw - L->pw) / 2;
        L->py[i] = L->cy[i] + (L->ch - L->ph) / 2;
    }
}

// Build the nearest-neighbour source-index LUTs for one src size. Called once
// (all quadrants share a size / src size). Removes a divide+multiply per pixel.
static int layout_build_luts(struct layout *L, int srcw, int srch)
{
    if (L->sx_lut && L->lut_srcw == srcw && L->lut_srch == srch) return 0;
    free(L->sx_lut); free(L->sy_lut);
    L->sx_lut = malloc(sizeof(int) * L->pw);
    L->sy_lut = malloc(sizeof(int) * L->ph);
    if (!L->sx_lut || !L->sy_lut) {
        fprintf(stderr, "layout: out of memory for LUTs\n");
        free(L->sx_lut); free(L->sy_lut);
        L->sx_lut = L->sy_lut = NULL;
        return -1;
    }
    for (int i = 0; i < L->pw; i++) {
        int v = (int)((long)i * srcw / L->pw);
        if (v > srcw - 1) v = srcw - 1;
        L->sx_lut[i] = v;
    }
    for (int j = 0; j < L->ph; j++) {
        int v = (int)((long)j * srch / L->ph);
        if (v > srch - 1) v = srch - 1;
        L->sy_lut[j] = v;
    }
    L->lut_srcw = srcw; L->lut_srch = srch;
    return 0;
}

// ===========================================================================
// Status readout
// ===========================================================================
// Recompute fps/live for every cam and print a one-line-per-node summary. dt_ms
// is the actual elapsed time of the window. 'out' is stderr (display) or stdout
// (probe). Resets the per-window counters.
static void status_tick(struct cam *cams, int ncam, long dt_ms, FILE *out)
{
    for (int i = 0; i < ncam; i++) {
        struct cam *c = &cams[i];
        if (!c->opened) { c->live = 0; c->fps = 0; continue; }
        c->fps  = dt_ms > 0 ? c->frames_window * 1000.0 / dt_ms : 0.0;
        c->live = c->frames_window > 0;
    }
    for (int i = 0; i < ncam; i++) {
        struct cam *c = &cams[i];
        if (i) fputc(' ', out), fputc(' ', out);
        if (!c->opened)      fprintf(out, "video%d: NOT OPENED", c->node);
        else if (!c->live)   fprintf(out, "video%d: NO SIGNAL", c->node);
        else                 fprintf(out, "video%d: %.1f fps bu=%u[%u..%u] partial=%lu/%lu",
                                     c->node, c->fps, c->bu_last, c->bu_min, c->bu_max,
                                     c->bu_partial, c->frames_total);
    }
    fputc('\n', out);
    fflush(out);
    for (int i = 0; i < ncam; i++) cams[i].frames_window = 0;
}

// ===========================================================================
// Main loops
// ===========================================================================

// Headless probe: stream briefly, count frames, print per-node status. No DRM.
static int run_probe(struct cam *cams, int ncam)
{
    struct pollfd pfds[MAX_CAM];
    long start = now_ms(), last = start;
    printf("probing %d node(s) for %d ms...\n", ncam, PROBE_MS);
    fflush(stdout);

    while (!g_stop && now_ms() - start < PROBE_MS) {
        int npfd = 0;
        int map[MAX_CAM];
        for (int i = 0; i < ncam; i++) {
            if (!cams[i].opened) continue;
            pfds[npfd].fd = cams[i].fd;
            pfds[npfd].events = POLLIN;
            pfds[npfd].revents = 0;
            map[npfd] = i;
            npfd++;
        }
        if (npfd == 0) break; // nothing opened; nothing to wait on

        int r = poll(pfds, npfd, POLL_TIMEOUT_MS);
        if (r < 0) { if (errno == EINTR) break; perror("poll"); break; }
        for (int k = 0; k < npfd; k++) {
            if (pfds[k].revents & (POLLIN | POLLERR)) {
                struct cam *c = &cams[map[k]];
                int idx = cam_drain_newest(c);
                if (idx >= 0) cam_qbuf(c, idx); // don't need the pixels, recycle
            }
        }
        long t = now_ms();
        if (t - last >= TICK_MS) { status_tick(cams, ncam, t - last, stdout); last = t; }
    }

    // Final summary reflects the whole probe window.
    printf("--- probe result ---\n");
    for (int i = 0; i < ncam; i++) {
        struct cam *c = &cams[i];
        if (!c->opened)
            printf("video%d: NOT OPENED\n", c->node);
        else if (c->frames_total == 0)
            printf("video%d: NO SIGNAL (0 frames)\n", c->node);
        else
            printf("video%d: OK, %lu frames total\n", c->node, c->frames_total);
    }
    fflush(stdout);
    return 0;
}

// Fullscreen display loop.
static int run_display(struct cam *cams, int ncam, struct drm_dev *d, struct layout *L)
{
    // Initial paint: every cell black (letterbox bars stay this color) + NO-SIG
    // overlay until frames arrive.
    for (int i = 0; i < ncam; i++) {
        fb_fill(d, L->cx[i], L->cy[i], L->cw, L->ch, 0x00000000u);
        overlay_status(d, L, i, cams[i].node, 0);
    }

    struct pollfd pfds[MAX_CAM];
    long last = now_ms();

    while (!g_stop) {
        int npfd = 0;
        int map[MAX_CAM];
        for (int i = 0; i < ncam; i++) {
            if (!cams[i].opened) continue;
            pfds[npfd].fd = cams[i].fd;
            pfds[npfd].events = POLLIN;
            pfds[npfd].revents = 0;
            map[npfd] = i;
            npfd++;
        }

        // Even with zero opened nodes, keep looping so the status tick runs and
        // the NO-SIGNAL overlays stay drawn; poll(NULL,0,timeout) just sleeps.
        int r = poll(npfd ? pfds : NULL, npfd, POLL_TIMEOUT_MS);
        if (r < 0) { if (errno == EINTR) break; perror("poll"); break; }

        for (int k = 0; k < npfd; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLERR))) continue;
            int slot = map[k];
            struct cam *c = &cams[slot];
            int idx = cam_drain_newest(c);
            if (idx < 0) continue;
            // Build/refresh the scaling LUTs for this src size (once). If that
            // allocation fails, requeue and skip rather than deref a NULL LUT.
            if (layout_build_luts(L, c->width, c->height) != 0) { cam_qbuf(c, idx); continue; }
            uyvy_to_xrgb_scaled(d, c->buf[idx].start, c->bytesperline,
                                L->px[slot], L->py[slot], L);
            overlay_status(d, L, slot, c->node, 1); // fresh frame -> green
            // One-shot raw dump from the live stream (after warmup) so the saved
            // bytes are exactly the frame being displayed. Always writes the full
            // sizeimage so the garbage region is captured too, for shear analysis.
            if (g_dump_path && !g_dump_done && c->frames_total > 20) {
                int fd = open(g_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    ssize_t wr = write(fd, c->buf[idx].start, c->sizeimage);
                    close(fd);
                    fprintf(stderr, "dumped %zd/%u bytes (bytesused=%u) from %s to %s\n",
                            wr, c->sizeimage, c->bu_last, c->path, g_dump_path);
                } else {
                    fprintf(stderr, "dump: open(%s): %s\n", g_dump_path, strerror(errno));
                }
                g_dump_done = 1;
            }
            cam_qbuf(c, idx);
        }

        long t = now_ms();
        if (t - last >= TICK_MS) {
            status_tick(cams, ncam, t - last, stderr);
            // A node that just went dark must not keep a stale frame on screen —
            // a frozen frame that looks alive defeats the purpose of the tool.
            for (int i = 0; i < ncam; i++) {
                if (!cams[i].live) {
                    fb_fill(d, L->cx[i], L->cy[i], L->cw, L->ch, 0x00000000u);
                    overlay_status(d, L, i, cams[i].node, 0);
                }
            }
            last = t;
        }
    }
    return 0;
}

// ===========================================================================
// CLI
// ===========================================================================
static void usage(const char *prog)
{
    printf(
"Usage: %s [options]\n"
"  Capture UYVY 1920x1080 from /dev/video0..N-1 and show a fullscreen 2x2 grid\n"
"  on the primary DRM connector. Per-node fps / NO-SIGNAL readout to stderr.\n"
"\n"
"Options:\n"
"  --single N          Show only /dev/videoN, fullscreen (no grid).\n"
"  --probe             Headless: stream briefly, print per-node status, exit.\n"
"                      (No DRM; safe without stopping ultima-app.service.)\n"
"  --verbose, -v       Print every frame's bytesused (short-frame diagnosis).\n"
"  --dump PATH         Write one settled full raw buffer to PATH, then keep going.\n"
"  --device PATH       DRM device (default /dev/dri/card0).\n"
"  --count N           Number of nodes /dev/video0..N-1 (default %d).\n"
"  --width N           Capture width override (default %d).\n"
"  --height N          Capture height override (default %d).\n"
"  --help              This message.\n"
"\n"
"NOTE: display modes require exclusive use of the display — first run:\n"
"        systemctl stop ultima-app.service\n",
        prog, DEF_COUNT, DEF_WIDTH, DEF_HEIGHT);
}

int main(int argc, char **argv)
{
    const char *drm_path = "/dev/dri/card0";
    int   count  = DEF_COUNT;
    int   single = -1;
    int   probe  = 0;
    uint32_t width = DEF_WIDTH, height = DEF_HEIGHT;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--probe"))  probe = 1;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--dump")    && i + 1 < argc) g_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--single")  && i + 1 < argc) single = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device")  && i + 1 < argc) drm_path = argv[++i];
        else if (!strcmp(argv[i], "--count")   && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width")   && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height")  && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
        else { fprintf(stderr, "unknown/incomplete arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (count < 1) count = 1;
    if (count > MAX_CAM) count = MAX_CAM;

    // SIGINT/SIGTERM without SA_RESTART so poll()/ioctl() return EINTR.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;   // sa_flags = 0 -> no SA_RESTART
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    // We never alter terminal modes (raw mode, etc.), so there is nothing to
    // "restore" on exit beyond flushing our own output.

    // Build the node list. --single overrides to a single node, fullscreen.
    struct cam cams[MAX_CAM];
    memset(cams, 0, sizeof cams);
    int ncam;
    if (single >= 0) {
        ncam = 1;
        cams[0].node = single;
    } else {
        ncam = count;
        for (int i = 0; i < ncam; i++) cams[i].node = i;
    }
    for (int i = 0; i < ncam; i++) {
        cams[i].fd = -1;
        snprintf(cams[i].path, sizeof cams[i].path, "/dev/video%d", cams[i].node);
    }

    // Open all nodes. A failure marks that node dead but never aborts.
    int opened = 0;
    for (int i = 0; i < ncam; i++) {
        if (cam_open(&cams[i], width, height) == 0) {
            opened++;
            printf("%s: OPEN  fmt=%.4s %ux%u bpl=%u sizeimage=%u buffers=%u\n",
                   cams[i].path, (char *)&cams[i].pixfmt, cams[i].width,
                   cams[i].height, cams[i].bytesperline, cams[i].sizeimage,
                   cams[i].n_buffers);
        } else {
            printf("%s: FAILED to open/configure (shown as dead quadrant)\n",
                   cams[i].path);
        }
    }
    fflush(stdout);

    int rc = 0;

    if (probe) {
        rc = run_probe(cams, ncam);
    } else {
        if (opened == 0)
            fprintf(stderr, "warning: no nodes opened; showing an all-dead grid\n");
        struct drm_dev d;
        if (drm_init(&d, drm_path) != 0) {
            fprintf(stderr, "DRM init failed; try --probe for a headless check\n");
            rc = 1;
        } else {
            struct layout L;
            memset(&L, 0, sizeof L);
            // Preserve aspect ratio using the real negotiated source size (fall
            // back to the requested size if nothing opened).
            int sw = (int)width, sh = (int)height;
            for (int i = 0; i < ncam; i++)
                if (cams[i].opened) { sw = (int)cams[i].width; sh = (int)cams[i].height; break; }
            layout_compute(&L, ncam, d.mode.hdisplay, d.mode.vdisplay, sw, sh);
            rc = run_display(cams, ncam, &d, &L);
            free(L.sx_lut);
            free(L.sy_lut);
            drm_cleanup(&d);
        }
    }

    for (int i = 0; i < ncam; i++) cam_close(&cams[i]);
    fprintf(stderr, "camqa: exiting\n");
    return rc;
}
