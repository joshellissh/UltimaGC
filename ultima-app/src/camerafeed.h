#ifndef CAMERAFEED_H
#define CAMERAFEED_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QTimer>
#include <QSocketNotifier>

// Captures live video from a USB UVC capture card (a MacroSilicon MS210x
// AV-to-USB grabber on the bench unit, confirmed via its USB descriptors —
// see beagleplay-falcon/NOTES.md) and exposes decoded frames as QImages for
// CameraView to render. Deliberately mirrors CanBus's shape (raw fd +
// QSocketNotifier + retry timer, see canbus.cpp) rather than inventing a
// new one.
//
// Lazily opened: the device is only opened while active is true, driven by
// Camera360Screen's open()/close(). This project measures first-Qt-frame to
// the millisecond (see NOTES.md's measure-boot.sh); opening a USB device and
// negotiating a streaming format at construction would regress that for a
// screen most drives never open.
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
    explicit CameraFeed(const QString &device = QStringLiteral("/dev/video0"),
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
    int m_frameWidth = 0;
    int m_frameHeight = 0;
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
#endif
};

#endif
