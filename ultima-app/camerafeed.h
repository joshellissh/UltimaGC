#ifndef CAMERAFEED_H
#define CAMERAFEED_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QTimer>
#include <QSocketNotifier>

// Captures live video from a V4L2 capture node and exposes decoded frames as
// QImages for CameraView to render. Originally a USB UVC capture card (a
// MacroSilicon MS210x AV-to-USB grabber, confirmed via its USB descriptors —
// see beagleplay-falcon/NOTES.md); now one of 4 instances pointed at
// /dev/mycam/cam1..4, the mycam004m driver's stable symlinks (fake or real
// backend — see ~/code/mycam004m/docs/ultima-app-integration.md). The class
// itself didn't need to change for that move: it was already just "open a
// device path, negotiate a format, mmap N buffers" with no UVC-specific
// logic. Deliberately mirrors CanBus's shape (raw fd + QSocketNotifier +
// retry timer, see canbus.cpp) rather than inventing a new one.
//
// Lazily opened: the device is only opened while active is true, driven by
// Camera360Screen's open()/close(). This project measures first-Qt-frame to
// the millisecond (see NOTES.md's measure-boot.sh); opening a capture device
// and negotiating a streaming format at construction would regress that for
// a screen most drives never open.
class CameraFeed : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // streaming and failed are deliberately separate: streaming only goes
    // true once a first frame has actually arrived (tens to hundreds of ms
    // after active is set, longer if the device is still enumerating), so
    // "not streaming yet" and "genuinely failed" need different QML
    // treatment — see Camera360Screen.qml.
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY failedChanged)
    // Negotiated frame size (VIDIOC_S_FMT is a negotiation, not a command —
    // this is what the driver actually granted). 0x0 until the first
    // successful format negotiation. QML derives the pillarbox aspect from
    // this rather than assuming 4:3, so a PAL 720x576 source renders
    // correctly too.
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY formatChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY formatChanged)

public:
    explicit CameraFeed(const QString &device = QStringLiteral("/dev/mycam/cam1"),
                         QObject *parent = nullptr);
    ~CameraFeed();

    bool active() const { return m_active; }
    void setActive(bool on);

    bool streaming() const { return m_streaming; }
    bool failed() const { return m_failed; }
    int frameWidth() const { return m_frameWidth; }
    int frameHeight() const { return m_frameHeight; }

    // Latest decoded frame. Called from CameraView::updatePaintNode() on the
    // render thread — safe without a lock only because capture and
    // conversion (onReadable(), in the .cpp) run on the GUI thread, which is
    // blocked during the sync phase that precedes updatePaintNode(). If
    // capture ever moves to a worker thread, this needs a QMutex guarding
    // m_frame — don't drop one silently if that change happens.
    QImage currentFrame() const { return m_frame; }

signals:
    void activeChanged();
    void streamingChanged();
    void failedChanged();
    void formatChanged();
    void frameReady();

private slots:
    void onReadable();
    void tryOpen();
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    void simulateTick();
#endif

private:
    void closeDevice();
    void setStreaming(bool on);
    void setFailed(bool on);

    QString m_device;
    bool m_active = false;
    bool m_streaming = false;
    bool m_failed = false;
    // Decoded/exposed size — what frameWidth()/frameHeight() report and what
    // m_frame is actually allocated at. NOT the same as what the driver
    // negotiated: see kDecimation in camerafeed.cpp for why these are
    // smaller than the raw capture size on the real V4L2 path.
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    // Raw driver-granted capture size/stride (VIDIOC_S_FMT's actual grant) —
    // internal only, used for the bytesused guard and row addressing in
    // onReadable()/convertUYVYToRGB32. Real V4L2 path only; the simulated
    // path (simulateTick()) has no separate capture size, it writes m_frame
    // directly at m_frameWidth/m_frameHeight.
    int m_captureWidth = 0;
    int m_captureHeight = 0;
    int m_bytesPerLine = 0;  // driver-granted V4L2 stride — see camerafeed.cpp's tryOpen()
    QImage m_frame;

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_reconnectTimer;

#ifdef __linux__
    // mmap'd V4L2 capture buffers (VIDIOC_REQBUFS/QUERYBUF/QBUF/DQBUF).
    static constexpr int kNumBuffers = 4;
    struct MappedBuffer { void *start = nullptr; size_t length = 0; };
    MappedBuffer m_buffers[kNumBuffers];
    int m_numBuffersMapped = 0;
    void unmapBuffers();
#endif

#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    // Dev-build stand-in (macOS always, Linux dev builds built with
    // CONFIG+=ultima_dev_sim — mirrors CanBus's own ULTIMA_SIMULATE guard).
    // A moving test pattern so Camera360Screen's layout/pillarbox/fallback
    // logic can be exercised without a capture card.
    QTimer m_simTimer;
    double m_simPhase = 0.0;
    // Dev-only real-photo override (ULTIMA_CAM_IMAGE_DIR) — see
    // simulateTick()'s comment in the .cpp. Loaded at most once per
    // instance; null means "unset, missing, or failed to decode", in
    // which case simulateTick() falls through to the procedural bars.
    QImage m_simStaticImage;
    bool m_simStaticImageLoadAttempted = false;
#endif
};

#endif
