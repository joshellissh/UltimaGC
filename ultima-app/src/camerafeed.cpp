#include "camerafeed.h"

#include <QtGlobal>
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
#include <linux/videodev2.h>
#endif

// Requested capture format — mycam004m's contract (both fake and real
// backends, see ~/code/mycam004m/docs/ultima-app-integration.md) is fixed
// YUYV 1920x1080@30, answered by coercion rather than negotiation: S_FMT
// hands back this exact format regardless of what's requested. The code
// below still reads back whatever the driver actually granted
// (m_frameWidth/m_frameHeight/m_bytesPerLine) rather than assuming these
// requested values landed — same reasoning as when this was a 720x480 UVC
// grabber, just now defending against a driver bug rather than a real
// negotiation.
static constexpr int kRequestedWidth = 1920;
static constexpr int kRequestedHeight = 1080;

CameraFeed::CameraFeed(const QString &device, QObject *parent)
    : QObject(parent), m_device(device)
{
    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CameraFeed::tryOpen);
    // Deliberately NOT calling tryOpen() here — the device stays closed
    // until setActive(true), driven by Camera360Screen opening. See the
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
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
        closeDevice();
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

#ifdef __linux__
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
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ::ioctl(m_fd, VIDIOC_STREAMOFF, &type); // best-effort
        unmapBuffers();
        ::close(m_fd);
        m_fd = -1;
    }
}
#else
void CameraFeed::closeDevice() {}
#endif

#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
void CameraFeed::tryOpen()
{
    if (!m_active || m_fd >= 0)
        return;

    int fd = ::open(m_device.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[camerafeed] open(%s): %s\n", qPrintable(m_device), strerror(errno));
        m_reconnectTimer.start();
        return;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_QUERYCAP: %s\n", strerror(errno));
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }
    // device_caps, not the device-wide capabilities field: a UVC grabber's
    // second node (metadata) reports VIDEO_CAPTURE in the union
    // "capabilities" field too, so only the per-node device_caps actually
    // discriminates the real capture node from it. See NOTES.md. mycam004m
    // has no second/metadata node on either backend (device_caps is plain
    // VIDEO_CAPTURE|STREAMING|READWRITE — see the integration doc), so this
    // check is dormant against these devices, but harmless to leave in
    // place rather than special-case away.
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
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (::ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_S_FMT: %s\n", strerror(errno));
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        fprintf(stderr, "[camerafeed] %s refused YUYV (got fourcc 0x%x) — unsupported\n",
                qPrintable(m_device), fmt.fmt.pix.pixelformat);
        ::close(fd);
        setFailed(true);
        return;
    }
    // Read back what was actually granted — S_FMT is a negotiation in
    // general, though mycam004m answers it by coercion (always hands back
    // 1920x1080 YUYV regardless of what's requested — see the integration
    // doc). Notably bytesperline: assuming it always equals width*2
    // (tightly packed, no stride padding) was flagged as a real risk during
    // the original UVC-grabber hardware bring-up (see beagleplay-falcon/
    // NOTES.md "Live camera feed") — reading past a stride the driver
    // actually padded would walk off the end of the mmap'd capture buffer,
    // a plausible SIGBUS source. Logged here so it's visible on real
    // hardware, not assumed.
    m_frameWidth = int(fmt.fmt.pix.width);
    m_frameHeight = int(fmt.fmt.pix.height);
    m_bytesPerLine = int(fmt.fmt.pix.bytesperline);
    fprintf(stderr, "[camerafeed] negotiated %dx%d bytesperline=%d sizeimage=%u\n",
            m_frameWidth, m_frameHeight, m_bytesPerLine, fmt.fmt.pix.sizeimage);
    emit formatChanged();

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = kNumBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "[camerafeed] VIDIOC_REQBUFS: %s\n", strerror(errno));
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }

    m_numBuffersMapped = 0;
    for (quint32 i = 0; i < req.count && i < quint32(kNumBuffers); ++i) {
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
        m_reconnectTimer.start();
        return;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[camerafeed] VIDIOC_STREAMON: %s\n", strerror(errno));
        unmapBuffers();
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }

    m_fd = fd;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &CameraFeed::onReadable);
    m_reconnectTimer.stop();
    fprintf(stderr, "[camerafeed] streaming %s: %dx%d YUYV\n",
            qPrintable(m_device), m_frameWidth, m_frameHeight);
}

// YUYV (YUY2) -> RGB32, standard BT.601 integer coefficients. Runs on the
// GUI thread (see camerafeed.h's currentFrame() comment) — with 4 instances
// now converting 1920x1080 each (vs. one 720x480 grabber before), this is
// the cost flagged as a real risk, not yet measured, in the integration
// doc's "one real risk worth flagging" section — check that before assuming
// this scales to 4 concurrent feeds.
static inline quint8 clamp255(int v) { return quint8(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// bytesPerLine is the driver-granted V4L2 stride (fmt.fmt.pix.bytesperline),
// NOT assumed to be width*2 — see the VIDIOC_S_FMT comment in tryOpen(). A
// wrong assumption here would walk source rows past the actual stride,
// eventually reading off the end of the mmap'd capture buffer.
static void convertYUYVToRGB32(const uchar *src, QImage &dst, int width, int height, int bytesPerLine)
{
    for (int y = 0; y < height; ++y) {
        const uchar *row = src + size_t(y) * bytesPerLine;
        QRgb *out = reinterpret_cast<QRgb *>(dst.scanLine(y));
        for (int x = 0; x < width; x += 2) {
            int y0 = row[0], u = row[1] - 128, y1 = row[2], v = row[3] - 128;
            row += 4;

            int rUV = (359 * v) >> 8;
            int gUV = (88 * u + 183 * v) >> 8;
            int bUV = (454 * u) >> 8;

            out[x]     = qRgb(clamp255(y0 + rUV), clamp255(y0 - gUV), clamp255(y0 + bUV));
            if (x + 1 < width)
                out[x + 1] = qRgb(clamp255(y1 + rUV), clamp255(y1 - gUV), clamp255(y1 + bUV));
        }
    }
}

void CameraFeed::onReadable()
{
    // Drain every buffer the driver has ready, but convert and emit only
    // the newest one. Capture can arrive faster than the GUI thread (where
    // this conversion runs — see camerafeed.h's threading note) can keep up
    // with rendering; converting each of up to kNumBuffers queued frames
    // per wake, only to have each overwrite m_frame before the next frame
    // is even displayed, would multiply that cost for nothing.
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
            closeDevice();
            setStreaming(false);
            m_reconnectTimer.start();
            return;
        }
        if (havePending) {
            // Superseded by this newer buffer — requeue without converting.
            ::ioctl(m_fd, VIDIOC_QBUF, &pending);
        }
        pending = buf;
        havePending = true;
    }
    if (!havePending)
        return; // spurious wake, nothing actually ready

    // bytesused guard: a short buffer (driver bug, or a mid-stream format
    // change this code doesn't handle) is a plausible failure mode, and
    // without this check it would render partly-stale rows from a previous
    // frame's leftover buffer contents instead of being skipped. Checked
    // against the driver's own stride (m_bytesPerLine), not an assumed
    // width*2 — see convertYUYVToRGB32.
    if (pending.index < quint32(m_numBuffersMapped) && m_frameWidth > 0 && m_frameHeight > 0
        && m_bytesPerLine > 0
        && pending.bytesused >= quint32(m_bytesPerLine) * quint32(m_frameHeight)) {
        QImage frame(m_frameWidth, m_frameHeight, QImage::Format_RGB32);
        convertYUYVToRGB32(static_cast<const uchar *>(m_buffers[pending.index].start),
                            frame, m_frameWidth, m_frameHeight, m_bytesPerLine);
        m_frame = frame;
        setStreaming(true);
        emit frameReady();
    }
    // Requeue promptly — holding buffers stalls the stream.
    ::ioctl(m_fd, VIDIOC_QBUF, &pending);
}
#else // !defined(__linux__) || defined(ULTIMA_SIMULATE)
void CameraFeed::tryOpen() {}
void CameraFeed::onReadable() {}

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
