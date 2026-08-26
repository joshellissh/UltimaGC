#include "camerafeed.h"

#include <QtGlobal>
#include <QThread>
#include <QMutexLocker>
#include <QMetaObject>
#include <stdio.h>
#include <string.h>

#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
#include <QPainter>
#include <QFont>
#include <QRandomGenerator>
#include <cmath>
#endif

#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <linux/videodev2.h>
#endif

// Requested capture format — mycam004m's contract (both fake and real
// backends, see ~/code/mycam004m/docs/ultima-app-integration.md) is fixed
// UYVY 1920x1080 (UYVY, not YUYV, since 2026-08-24: real hardware
// proved the N4 decoder emits UYVY byte order — decoding it as YUYV was
// the green/magenta mess of the first live capture), answered by
// coercion rather than negotiation: S_FMT hands back this exact format
// regardless of what's requested. The code below still reads back whatever
// the driver actually granted (m_captureWidth/m_captureHeight/
// m_bytesPerLine) rather than assuming these requested values landed —
// same reasoning as when this was a 720x480 UVC grabber, just now
// defending against a driver bug rather than a real negotiation.
static constexpr int kRequestedWidth = 1920;
static constexpr int kRequestedHeight = 1080;

CameraFeed::CameraFeed(const QString &device, QObject *parent)
    : QObject(parent), m_device(device)
{
    m_fpsLogEnabled = qEnvironmentVariableIsSet("ULTIMA_CAM_FPS_LOG");
    // Zero-copy display is the default on the real capture path — see the
    // class comment and NOTES.md "Camera framerate". ULTIMA_CAM_ZEROCOPY=0
    // is the escape hatch that forces everything through the converted-
    // QImage path (useful A/B lever, and the fallback if this ever runs on
    // a GPU stack without dma-buf import).
    const QByteArray zc = qgetenv("ULTIMA_CAM_ZEROCOPY");
    m_zeroCopy = zc.isEmpty() ? true : (zc.toInt() != 0);
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    m_zeroCopy = false; // simulated frames are QImages; there is no V4L2 buffer to lend
#endif
    for (int i = 0; i < kMaxBuffers; ++i) {
        m_dmabufFds[i] = -1;
        m_bufRefs[i] = 0;
    }
    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CameraFeed::tryOpen);
    // Deliberately NOT calling tryOpen() here — the device stays closed
    // until setActive(true), driven by the camera screens opening. See the
    // class comment in camerafeed.h.
}

CameraFeed::~CameraFeed()
{
    closeDevice();
}

void CameraFeed::setActive(bool on)
{
    if (on == m_active)
        return;
    m_active = on;
    emit activeChanged();

    if (m_active) {
        setFailed(false);
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
        tryOpen();
#else
        connect(&m_simTimer, &QTimer::timeout, this, &CameraFeed::simulateTick,
                Qt::UniqueConnection);
        m_simTimer.start(33); // ~30fps, matches the real grabber's default rate
#endif
    } else {
        m_reconnectTimer.stop();
        m_reconnectIntervalMs = 1000; // start fresh next time this feed is activated
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
        closeDevice();
        m_frame = QImage(); // don't serve a stale frame across a close/open cycle
#else
        m_simTimer.stop();
#endif
        setStreaming(false);
    }
}

void CameraFeed::setStreaming(bool on)
{
    if (on == m_streaming)
        return;
    m_streaming = on;
    emit streamingChanged();
}

void CameraFeed::setFailed(bool on)
{
    if (on == m_failed)
        return;
    m_failed = on;
    emit failedChanged();
}

// A permanently-absent/non-streaming feed (e.g. a camera the current
// hardware/driver setup can't actually enable — see beagleplay-falcon/
// NOTES.md's "Still open" note on simultaneous multi-camera capture) used
// to retry tryOpen() every flat 1s forever. With multiple such feeds active
// at once (the camera screens activate all 4 unconditionally, regardless
// of which cameras currently work), that's several failed
// open+ioctl+mmap+close cycles per second competing for the same GUI
// thread — measured via strace on real hardware (2026-08-25) at ~1.2s of
// ioctl/mmap/close time per 8s wall-clock window from 3 permanently-
// failing feeds alone. Exponential backoff (capped, reset to base on the
// next successful stream) keeps a genuinely-dead feed from costing much
// after the first few seconds, while still reconnecting promptly if the
// camera comes back (e.g. a cable reseated, or the driver's multi-camera
// support lands).
void CameraFeed::scheduleReconnect()
{
    static constexpr int kMaxReconnectIntervalMs = 8000;
    m_reconnectTimer.setInterval(m_reconnectIntervalMs);
    m_reconnectTimer.start();
    m_reconnectIntervalMs = m_reconnectIntervalMs * 2 < kMaxReconnectIntervalMs
                                 ? m_reconnectIntervalMs * 2 : kMaxReconnectIntervalMs;
}

// ---- Zero-copy buffer mailbox --------------------------------------------
//
// The capture thread publishes the newest DQBUF'd buffer index; renderers
// (grid tiles, mirror overlays — possibly several per feed) acquire it with
// a reference during the scene-graph sync phase and release it when they
// move on to a newer one. A buffer goes onto m_retired — and from there
// back to the driver via VIDIOC_QBUF, always on the capture thread — only
// once it is BOTH superseded and unreferenced. Renderers hold their
// current AND previous buffer (see dmabuftexture.cpp) so the driver can
// never DMA into memory the GPU may still be scanning for the in-flight
// frame.

int CameraFeed::dmabufFd(int index) const
{
    QMutexLocker lock(&m_bufMutex);
    return (index >= 0 && index < kMaxBuffers) ? m_dmabufFds[index] : -1;
}

quint32 CameraFeed::bufferSession() const
{
    QMutexLocker lock(&m_bufMutex);
    return m_bufSession;
}

int CameraFeed::acquireLatestBuffer(quint32 *generation, quint32 *session)
{
    QMutexLocker lock(&m_bufMutex);
    if (m_pendingIndex < 0 || *generation == m_pendingGen)
        return -1;
    *generation = m_pendingGen;
    *session = m_bufSession;
    ++m_bufRefs[m_pendingIndex];
    m_pendingAcquired = true;
    return m_pendingIndex;
}

void CameraFeed::releaseBuffer(int index, quint32 session)
{
    QMutexLocker lock(&m_bufMutex);
    if (index < 0 || index >= kMaxBuffers || session != m_bufSession)
        return; // stale handle from a previous stream — nothing to give back
    if (m_bufRefs[index] > 0 && --m_bufRefs[index] == 0 && index != m_pendingIndex)
        m_retired.append(index);
}

void CameraFeed::publishBuffer(int index)
{
    QMutexLocker lock(&m_bufMutex);
    if (m_pendingIndex >= 0 && m_pendingIndex != index) {
        // Superseded. Never displayed, or displayed and since released by
        // everyone: retire it here. Otherwise releaseBuffer() retires it
        // when the last renderer lets go.
        if (!m_pendingAcquired || m_bufRefs[m_pendingIndex] == 0)
            m_retired.append(m_pendingIndex);
    }
    m_pendingIndex = index;
    m_pendingAcquired = false;
    ++m_pendingGen;
}

QVector<int> CameraFeed::takeRetiredBuffers()
{
    QMutexLocker lock(&m_bufMutex);
    QVector<int> r;
    r.swap(m_retired);
    return r;
}

void CameraFeed::resetBufferMailbox()
{
    QMutexLocker lock(&m_bufMutex);
    m_pendingIndex = -1;
    m_pendingAcquired = false;
    m_retired.clear();
    for (int i = 0; i < kMaxBuffers; ++i)
        m_bufRefs[i] = 0;
    ++m_bufSession; // outstanding handles (and any EGLImports of them) are now stale
}

#if defined(__linux__) && !defined(ULTIMA_SIMULATE)

// Decode/expose converted frames at 1/kDecimation the driver-granted
// capture size — real-hardware profiling (2026-08-17, see beagleplay-falcon/
// NOTES.md) found full-res conversion+upload spending most of its cost on
// pixels thrown away by the eventual on-screen downscale. Only affects the
// converted-QImage path; the zero-copy path displays the full capture
// resolution (the GPU minifies while sampling).
static constexpr int kDecimation = 2;

// UYVY -> RGBA8888, standard BT.601 integer coefficients, decimating by
// rowStep/colStep pixel-PAIRS (never splitting a pair keeps every read
// aligned to UYVY's 4-byte/2-pixel chroma grouping). bytesPerLine is the
// driver-granted V4L2 stride, NOT assumed to be width*2 — walking rows past
// the real stride would read off the end of the buffer (see tryOpen()).
//
// Portable scalar fallback — the NEON version below is what the target
// actually runs. Kept bit-compatible in structure (not bit-exact: the NEON
// coefficients are the same values halved for int16 headroom, ~0.4% off).
static inline quint8 clamp255(int v) { return quint8(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static void convertUYVYToRGBA8888(const uchar *src, QImage &dst, int dstWidth, int dstHeight,
                                   int bytesPerLine, int rowStep, int colStep)
{
    for (int y = 0; y < dstHeight; ++y) {
        const uchar *row = src + size_t(y) * rowStep * bytesPerLine;
        uchar *out = dst.scanLine(y);
        for (int x = 0; x < dstWidth; x += 2) {
            const uchar *px = row + size_t(x / 2) * colStep * 4;
            int u = px[0] - 128, y0 = px[1], v = px[2] - 128, y1 = px[3];

            int rUV = (359 * v) >> 8;
            int gUV = (88 * u + 183 * v) >> 8;
            int bUV = (454 * u) >> 8;

            uchar *p0 = out + size_t(x) * 4;
            p0[0] = clamp255(y0 + rUV);
            p0[1] = clamp255(y0 - gUV);
            p0[2] = clamp255(y0 + bUV);
            p0[3] = 255;
            if (x + 1 < dstWidth) {
                uchar *p1 = p0 + 4;
                p1[0] = clamp255(y1 + rUV);
                p1[1] = clamp255(y1 - gUV);
                p1[2] = clamp255(y1 + bUV);
                p1[3] = 255;
            }
        }
    }
}

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
// NEON UYVY->RGBA8888 with fixed column decimation 2. Each iteration pulls
// 64 contiguous source bytes (16 UYVY pixel pairs) with ONE vld4q_u8 —
// de-interleaved straight into U / Y0 / V / Y1 lanes — keeps every other
// pair (8 pairs = 16 output pixels) and writes 64 output bytes with one
// vst4q_u8. Hardware-measured (2026-08-26) at ~3.6ms/frame vs the scalar
// loop's ~8.9ms from cached buffers — and this wide-load shape is also
// what makes the conversion survivable at all if the buffers ever come
// back uncached (see the NON_COHERENT note in tryOpen(): on an uncached
// mapping every load is a DRAM round-trip regardless of width, so 64-byte
// loads vs the scalar path's 1-byte loads is the difference between 64ms
// and 240ms a frame). Coefficients are the scalar path's halved with >>7
// instead of >>8 so the intermediates fit int16 lanes (359*127 doesn't).
static void convertUYVYToRGBA8888Neon(const uchar *src, QImage &dst, int dstWidth, int dstHeight,
                                       int bytesPerLine, int rowStep)
{
    const int16x8_t c128 = vdupq_n_s16(128);
    const int16x8_t cR = vdupq_n_s16(179);   // 359/2
    const int16x8_t cGu = vdupq_n_s16(44);   // 88/2
    const int16x8_t cGv = vdupq_n_s16(91);   // 183/2 (rounded down)
    const int16x8_t cB = vdupq_n_s16(227);   // 454/2
    const uint8x16_t alpha = vdupq_n_u8(255);
    const int vecWidth = dstWidth & ~15;
    for (int y = 0; y < dstHeight; ++y) {
        const uchar *row = src + size_t(y) * rowStep * bytesPerLine;
        uchar *out = dst.scanLine(y);
        int x = 0;
        for (; x < vecWidth; x += 16) {
            // 16 source pairs (64 bytes) -> keep even-indexed pairs (8).
            uint8x16x4_t q = vld4q_u8(row + size_t(x) * 4); // x px out = 2x px in = 4x bytes
            uint8x8_t U  = vget_low_u8(vuzp1q_u8(q.val[0], q.val[0]));
            uint8x8_t Y0 = vget_low_u8(vuzp1q_u8(q.val[1], q.val[1]));
            uint8x8_t V  = vget_low_u8(vuzp1q_u8(q.val[2], q.val[2]));
            uint8x8_t Y1 = vget_low_u8(vuzp1q_u8(q.val[3], q.val[3]));

            int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(U)), c128);
            int16x8_t v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(V)), c128);
            int16x8_t y0 = vreinterpretq_s16_u16(vmovl_u8(Y0));
            int16x8_t y1 = vreinterpretq_s16_u16(vmovl_u8(Y1));

            int16x8_t rUV = vshrq_n_s16(vmulq_s16(cR, v), 7);
            int16x8_t gUV = vshrq_n_s16(vaddq_s16(vmulq_s16(cGu, u), vmulq_s16(cGv, v)), 7);
            int16x8_t bUV = vshrq_n_s16(vmulq_s16(cB, u), 7);

            uint8x8_t r0 = vqmovun_s16(vaddq_s16(y0, rUV));
            uint8x8_t g0 = vqmovun_s16(vsubq_s16(y0, gUV));
            uint8x8_t b0 = vqmovun_s16(vaddq_s16(y0, bUV));
            uint8x8_t r1 = vqmovun_s16(vaddq_s16(y1, rUV));
            uint8x8_t g1 = vqmovun_s16(vsubq_s16(y1, gUV));
            uint8x8_t b1 = vqmovun_s16(vaddq_s16(y1, bUV));

            // Interleave even/odd pixels of each pair back into raster order.
            uint8x16x4_t o;
            o.val[0] = vcombine_u8(vzip1_u8(r0, r1), vzip2_u8(r0, r1));
            o.val[1] = vcombine_u8(vzip1_u8(g0, g1), vzip2_u8(g0, g1));
            o.val[2] = vcombine_u8(vzip1_u8(b0, b1), vzip2_u8(b0, b1));
            o.val[3] = alpha;
            vst4q_u8(out + size_t(x) * 4, o);
        }
        // Scalar tail for widths that aren't a multiple of 16.
        if (x < dstWidth) {
            // Reuse the scalar loop on the remaining columns of this row.
            const uchar *tailRow = row;
            uchar *tailOut = out;
            for (; x < dstWidth; x += 2) {
                const uchar *px = tailRow + size_t(x / 2) * 2 * 4;
                int u = px[0] - 128, y0 = px[1], v = px[2] - 128, y1 = px[3];
                int rUV = (359 * v) >> 8, gUV = (88 * u + 183 * v) >> 8, bUV = (454 * u) >> 8;
                uchar *p0 = tailOut + size_t(x) * 4;
                p0[0] = clamp255(y0 + rUV); p0[1] = clamp255(y0 - gUV); p0[2] = clamp255(y0 + bUV); p0[3] = 255;
                if (x + 1 < dstWidth) {
                    uchar *p1 = p0 + 4;
                    p1[0] = clamp255(y1 + rUV); p1[1] = clamp255(y1 - gUV); p1[2] = clamp255(y1 + bUV); p1[3] = 255;
                }
            }
        }
    }
}
#define ULTIMA_HAVE_NEON 1
#endif

// Per-feed capture thread. Owns nothing: the fd and mmap'd buffers stay
// with CameraFeed (opened/closed on the GUI thread); this thread polls the
// fd, DQBUFs with a drain-all-take-newest policy (capture can outpace
// display; converting frames that would be overwritten before ever being
// shown is pure waste), then:
//   - zero-copy path: publishes the buffer index to the mailbox and
//     requeues whatever the renderers have retired;
//   - converted path (zero-copy off, or a frame consumer registered):
//     decodes UYVY->RGBA and requeues the buffer immediately.
// Converted frames reach the GUI thread through a latest-frame mailbox
// rather than a queued signal per frame: if the GUI thread is blocked
// (e.g. waiting on the render thread's sync), frames overwrite each other
// here instead of piling up in the event queue as 2MB QImages.
class CameraCaptureThread : public QThread {
public:
    CameraCaptureThread(CameraFeed *feed, int fd, bool fpsLog)
        : m_feed(feed), m_fd(fd), m_fpsLog(fpsLog) {}

    void requestStop() { m_stop.storeRelease(1); }

    QImage takeLatest() {
        QMutexLocker lock(&m_mutex);
        QImage f = m_latest;
        m_latest = QImage();
        m_notifyPending.storeRelease(0);
        return f;
    }

protected:
    void run() override {
        QElapsedTimer fpsTimer, stage;
        int arrived = 0, published = 0, decoded = 0;
        qint64 convNs = 0, convMax = 0;
        fpsTimer.start();
        while (!m_stop.loadAcquire()) {
            struct pollfd pfd;
            pfd.fd = m_fd; pfd.events = POLLIN; pfd.revents = 0;
            int r = ::poll(&pfd, 1, 100);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) continue;
            if (m_stop.loadAcquire()) break;

            bool havePending = false;
            struct v4l2_buffer pending;
            memset(&pending, 0, sizeof(pending));
            for (;;) {
                struct v4l2_buffer buf;
                memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                if (::ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
                    if (errno == EAGAIN)
                        break;
                    fprintf(stderr, "[camerafeed] VIDIOC_DQBUF: %s — reconnecting\n", strerror(errno));
                    QMetaObject::invokeMethod(m_feed, "onWorkerError", Qt::QueuedConnection);
                    return; // the GUI thread will closeDevice() and wait() on us
                }
                ++arrived;
                if (havePending)
                    ::ioctl(m_fd, VIDIOC_QBUF, &pending); // superseded before we ever looked at it
                pending = buf;
                havePending = true;
            }
            if (!havePending)
                continue;

            const bool zeroCopy = m_feed->m_zeroCopy;
            const bool wantImage = !zeroCopy || m_feed->m_frameConsumers.loadAcquire() > 0;

            // bytesused guard: a short buffer (driver bug, or a mid-stream
            // format change this code doesn't handle) would otherwise show
            // partly-stale rows from a previous frame's leftover contents.
            const bool complete = pending.index < quint32(m_feed->m_numBuffersMapped)
                && m_feed->m_bytesPerLine > 0
                && pending.bytesused >= quint32(m_feed->m_bytesPerLine) * quint32(m_feed->m_captureHeight);

            if (complete && wantImage) {
                stage.start();
                QImage frame(m_feed->m_frameWidth, m_feed->m_frameHeight, QImage::Format_RGBA8888);
                const uchar *src = static_cast<const uchar *>(m_feed->m_buffers[pending.index].start);
#ifdef ULTIMA_HAVE_NEON
                convertUYVYToRGBA8888Neon(src, frame, m_feed->m_frameWidth, m_feed->m_frameHeight,
                                          m_feed->m_bytesPerLine, kDecimation);
#else
                convertUYVYToRGBA8888(src, frame, m_feed->m_frameWidth, m_feed->m_frameHeight,
                                      m_feed->m_bytesPerLine, kDecimation, kDecimation);
#endif
                const qint64 ns = stage.nsecsElapsed();
                convNs += ns;
                if (ns > convMax) convMax = ns;
                ++decoded;
                {
                    QMutexLocker lock(&m_mutex);
                    m_latest = frame;
                }
            }

            if (complete && zeroCopy) {
                m_feed->publishBuffer(int(pending.index));
                ++published;
            } else {
                ::ioctl(m_fd, VIDIOC_QBUF, &pending);
            }
            if (complete && m_notifyPending.testAndSetAcquire(0, 1))
                QMetaObject::invokeMethod(m_feed, "onWorkerFrame", Qt::QueuedConnection);

            if (zeroCopy) {
                const QVector<int> retired = m_feed->takeRetiredBuffers();
                for (int idx : retired) {
                    struct v4l2_buffer rb;
                    memset(&rb, 0, sizeof(rb));
                    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    rb.memory = V4L2_MEMORY_MMAP;
                    rb.index = quint32(idx);
                    if (::ioctl(m_fd, VIDIOC_QBUF, &rb) < 0)
                        fprintf(stderr, "[camerafeed] requeue[%d]: %s\n", idx, strerror(errno));
                }
            }

            if (m_fpsLog && fpsTimer.elapsed() >= 2000) {
                const double el = fpsTimer.elapsed(), n = decoded > 0 ? decoded : 1;
                fprintf(stderr, "[camerafeed] %s: %.1f fps arrived, %.1f published, %.1f decoded | convert %.2f ms avg / %.2f ms max\n",
                        qPrintable(m_feed->m_device), arrived * 1000.0 / el,
                        published * 1000.0 / el, decoded * 1000.0 / el,
                        convNs / n / 1e6, convMax / 1e6);
                arrived = published = decoded = 0;
                convNs = convMax = 0;
                fpsTimer.restart();
            }
        }
    }

private:
    CameraFeed *m_feed;
    int m_fd;
    bool m_fpsLog;
    QAtomicInt m_stop{0};
    QAtomicInt m_notifyPending{0};
    QMutex m_mutex;
    QImage m_latest;
};

void CameraFeed::onWorkerFrame()
{
    if (!m_captureThread)
        return;
    QImage f = m_captureThread->takeLatest();
    if (!f.isNull())
        m_frame = f; // GUI thread — see currentFrame()'s threading note
    setStreaming(true);
    emit frameReady();
}

void CameraFeed::onWorkerError()
{
    closeDevice();
    setStreaming(false);
    scheduleReconnect();
}

void CameraFeed::unmapBuffers()
{
    for (int i = 0; i < m_numBuffersMapped; ++i) {
        if (m_buffers[i].start && m_buffers[i].start != MAP_FAILED)
            ::munmap(m_buffers[i].start, m_buffers[i].length);
        m_buffers[i] = MappedBuffer();
    }
    m_numBuffersMapped = 0;
}

void CameraFeed::closeDevice()
{
    if (m_captureThread) {
        m_captureThread->requestStop();
        m_captureThread->wait();
        delete m_captureThread;
        m_captureThread = nullptr;
    }
    if (m_fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ::ioctl(m_fd, VIDIOC_STREAMOFF, &type); // best-effort
        unmapBuffers();
        ::close(m_fd);
        m_fd = -1;
    }
    // The exported fds are closed here, but an EGLImage a renderer still
    // holds keeps the underlying buffer memory alive (dma-buf refcount)
    // until DmaBufTextureSet notices the session bump below and drops it —
    // see dmabuftexture.cpp for how that resolves against CMA pressure.
    for (int i = 0; i < kMaxBuffers; ++i) {
        if (m_dmabufFds[i] >= 0) {
            ::close(m_dmabufFds[i]);
            m_dmabufFds[i] = -1;
        }
    }
    resetBufferMailbox();
}

void CameraFeed::tryOpen()
{
    if (!m_active || m_fd >= 0)
        return;

    int fd = ::open(m_device.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[camerafeed] open(%s): %s\n", qPrintable(m_device), strerror(errno));
        scheduleReconnect();
        return;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_QUERYCAP: %s\n", strerror(errno));
        ::close(fd);
        scheduleReconnect();
        return;
    }
    // device_caps, not the device-wide capabilities field: a UVC grabber's
    // second node (metadata) reports VIDEO_CAPTURE in the union
    // "capabilities" field too, so only the per-node device_caps actually
    // discriminates the real capture node from it. See NOTES.md. mycam004m
    // has no second/metadata node on either backend, so this check is
    // dormant against these devices, but harmless to leave in place.
    quint32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                       ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[camerafeed] %s is not a streaming capture node (caps=0x%x)\n",
                qPrintable(m_device), caps);
        ::close(fd);
        setFailed(true);
        return; // not a transient condition — don't retry against the wrong node
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = kRequestedWidth;
    fmt.fmt.pix.height = kRequestedHeight;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (::ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_S_FMT: %s\n", strerror(errno));
        ::close(fd);
        scheduleReconnect();
        return;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY) {
        fprintf(stderr, "[camerafeed] %s refused UYVY (got fourcc 0x%x) — unsupported\n",
                qPrintable(m_device), fmt.fmt.pix.pixelformat);
        ::close(fd);
        setFailed(true);
        return;
    }
    // Read back what was actually granted — notably bytesperline: assuming
    // it always equals width*2 (tightly packed) was flagged as a real risk
    // during the original UVC-grabber bring-up (see beagleplay-falcon/
    // NOTES.md "Live camera feed") — reading past a stride the driver
    // actually padded would walk off the end of the capture buffer.
    m_captureWidth = int(fmt.fmt.pix.width);
    m_captureHeight = int(fmt.fmt.pix.height);
    m_bytesPerLine = int(fmt.fmt.pix.bytesperline);
    m_frameWidth = m_captureWidth / kDecimation;
    m_frameHeight = m_captureHeight / kDecimation;
    fprintf(stderr, "[camerafeed] negotiated %dx%d bytesperline=%d sizeimage=%u, decoding at %dx%d\n",
            m_captureWidth, m_captureHeight, m_bytesPerLine, fmt.fmt.pix.sizeimage,
            m_frameWidth, m_frameHeight);
    emit formatChanged();

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = m_zeroCopy ? 6 : 4; // see camerafeed.h's kMaxBuffers comment
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    // Ask vb2 for NON-COHERENT (CPU-cacheable) buffers instead of the
    // default coherent (uncached, Normal-NC) DMA mapping. This single flag
    // is what took the UYVY conversion from 240ms/frame (the "3fps camera
    // grid" bug, root-caused 2026-08-26 — every byte load from an uncached
    // mapping is a DRAM round-trip) to 3.6ms: vb2 cache-invalidates on
    // DQBUF itself, so reads hit cache at full speed. Only honored if the
    // driver opts in (q->allow_cache_hints — j721e-csi2rx does); harmless
    // no-op elsewhere, and the fps log's convert times make a silent
    // downgrade visible. The CPU never *writes* the buffers, so there are
    // no dirty lines to conflict with the DMA or the GPU's own reads.
#ifdef V4L2_MEMORY_FLAG_NON_COHERENT
    req.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
#endif
    if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "[camerafeed] VIDIOC_REQBUFS: %s\n", strerror(errno));
        ::close(fd);
        scheduleReconnect();
        return;
    }

    m_numBuffersMapped = 0;
    for (quint32 i = 0; i < req.count && i < quint32(kMaxBuffers); ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (::ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[camerafeed] VIDIOC_QUERYBUF[%u]: %s\n", i, strerror(errno));
            break;
        }
        void *start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (start == MAP_FAILED) {
            fprintf(stderr, "[camerafeed] mmap[%u]: %s\n", i, strerror(errno));
            break;
        }
        m_buffers[i].start = start;
        m_buffers[i].length = buf.length;
        m_numBuffersMapped = int(i) + 1;
        if (::ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[camerafeed] VIDIOC_QBUF[%u]: %s\n", i, strerror(errno));
            break;
        }
    }
    if (m_numBuffersMapped < 2) {
        unmapBuffers();
        ::close(fd);
        scheduleReconnect();
        return;
    }

    if (m_zeroCopy) {
        // Export every buffer as a dma-buf for the renderers' EGLImage
        // import (dmabuftexture.cpp). Failure here isn't fatal: fall back
        // to the converted-QImage path for this stream and say so loudly.
        QMutexLocker lock(&m_bufMutex);
        for (int i = 0; i < m_numBuffersMapped; ++i) {
            struct v4l2_exportbuffer exp;
            memset(&exp, 0, sizeof(exp));
            exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index = quint32(i);
            exp.flags = O_CLOEXEC | O_RDONLY;
            if (::ioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
                fprintf(stderr, "[camerafeed] VIDIOC_EXPBUF[%d]: %s — zero-copy off, converting instead\n",
                        i, strerror(errno));
                for (int j = 0; j < i; ++j) {
                    ::close(m_dmabufFds[j]);
                    m_dmabufFds[j] = -1;
                }
                m_zeroCopy = false;
                break;
            }
            m_dmabufFds[i] = exp.fd;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_STREAMON: %s\n", strerror(errno));
        for (int i = 0; i < kMaxBuffers; ++i) {
            if (m_dmabufFds[i] >= 0) { ::close(m_dmabufFds[i]); m_dmabufFds[i] = -1; }
        }
        unmapBuffers();
        ::close(fd);
        scheduleReconnect();
        return;
    }

    m_fd = fd;
    m_captureThread = new CameraCaptureThread(this, fd, m_fpsLogEnabled);
    m_captureThread->start();
    m_reconnectTimer.stop();
    m_reconnectIntervalMs = 1000; // back to base now that this feed is healthy
    fprintf(stderr, "[camerafeed] streaming %s: %dx%d UYVY (%d buffers, zerocopy=%d, reqbufs caps=0x%x flags=0x%x)\n",
            qPrintable(m_device), m_captureWidth, m_captureHeight, m_numBuffersMapped,
            int(m_zeroCopy), req.capabilities, req.flags);
}

#else // !defined(__linux__) || defined(ULTIMA_SIMULATE)

void CameraFeed::tryOpen() {}
void CameraFeed::onWorkerFrame() {}
void CameraFeed::onWorkerError() {}
#ifndef __linux__
void CameraFeed::closeDevice() {}
#else
void CameraFeed::unmapBuffers() {}
void CameraFeed::closeDevice() {}
#endif

// Dev-build stand-in: a moving test pattern (diagonal bars sliding
// sideways) so Camera360Screen's pillarbox/fallback/aspect logic can be
// exercised on scripts/dev-build.sh without capture hardware. Uses the same
// negotiated-size shape (frameWidth/frameHeight) real hardware would.
//
// The bar color is derived from m_device rather than fixed, and the device
// path is drawn as the label — 4 concurrent instances (one per
// /dev/mycam/camN) need to look visibly different from each other, the same
// way the real fake driver's reference images each use a distinct
// background + marker count (see ~/code/mycam004m/tools/gen_fake_frames.py)
// specifically so a cross-wired quadrant (cam2 showing in cam3's slot) is
// obvious by eye rather than needing pixel inspection.
void CameraFeed::simulateTick()
{
    if (m_frameWidth != kRequestedWidth || m_frameHeight != kRequestedHeight) {
        m_frameWidth = kRequestedWidth;
        m_frameHeight = kRequestedHeight;
        emit formatChanged();
    }

    // Dev-only override, same idea as main.qml's ULTIMA_SPLASH_IMAGE: if
    // ULTIMA_CAM_IMAGE_DIR is set, show a real static photo
    // (<dir>/<camN>.png, camN taken from m_device's last path component,
    // e.g. "cam3") instead of the bars below -- lets the macOS/non-Linux
    // build be pointed at the same real reference frames the on-target
    // fake driver serves (see ~/code/mycam004m/tools/gen_fake_frames.py),
    // without needing that Linux-only V4L2 path compiled in. Loaded once
    // and cached; unset/missing/undecodable all fall through to the bars.
    if (!m_simStaticImageLoadAttempted) {
        m_simStaticImageLoadAttempted = true;
        const QString dir = qEnvironmentVariable("ULTIMA_CAM_IMAGE_DIR");
        if (!dir.isEmpty()) {
            const QString camId = m_device.section(QLatin1Char('/'), -1);
            QImage img(dir + QLatin1Char('/') + camId + QStringLiteral(".png"));
            if (!img.isNull())
                m_simStaticImage = img.scaled(m_frameWidth, m_frameHeight,
                                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }
    if (!m_simStaticImage.isNull()) {
        m_frame = m_simStaticImage;
        setStreaming(true);
        emit frameReady();
        return;
    }

    m_simPhase += 4.0;
    QImage frame(m_frameWidth, m_frameHeight, QImage::Format_RGB32);
    QPainter p(&frame);
    p.fillRect(frame.rect(), QColor(20, 20, 24));
    p.setPen(Qt::NoPen);
    const uint h = qHash(m_device);
    p.setBrush(QColor(80 + h % 150, 80 + (h / 150) % 150, 80 + (h / 22500) % 150));
    const int barWidth = 40;
    for (int x = -barWidth; x < frame.width() + barWidth; x += barWidth * 2) {
        int shifted = int(std::fmod(x + m_simPhase, frame.width() + 2.0 * barWidth)) - barWidth;
        p.drawRect(shifted, 0, barWidth, frame.height());
    }
    p.setPen(Qt::white);
    QFont font = p.font();
    font.setPointSize(48);
    p.setFont(font);
    p.drawText(frame.rect(), Qt::AlignCenter,
               QStringLiteral("%1\n(simulated)").arg(m_device));
    p.end();

    m_frame = frame;
    setStreaming(true);
    emit frameReady();
}
#endif
