#ifndef CAMERAFEED_H
#define CAMERAFEED_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <QVector>
#include <QAtomicInt>

// Captures live video from a V4L2 capture node. Originally a USB UVC capture
// card (a MacroSilicon MS210x AV-to-USB grabber, confirmed via its USB
// descriptors — see beagleplay-falcon/NOTES.md); now one of 4 instances
// pointed at /dev/mycam/cam1..4, the mycam004m driver's stable symlinks
// (fake or real backend — see ~/code/mycam004m/docs/ultima-app-integration.md).
//
// Frames reach the two consumers over two parallel paths (see
// beagleplay-falcon/NOTES.md "Camera framerate" for the 2026-08-26
// hardware profiling that shaped this):
//
// 1. ZERO-COPY (the display path — CameraView's grid tiles and mirror
//    overlays). Each capture buffer is exported as a dma-buf
//    (VIDIOC_EXPBUF) and imported by the renderer as an EGLImage-backed
//    GL_TEXTURE_EXTERNAL_OES (see dmabuftexture.h); the GPU samples the
//    UYVY buffer directly and no pixel is ever touched by the CPU. The
//    acquire/release mailbox below lends buffer indices to renderers and
//    returns them to the driver (VIDIOC_QBUF) only when superseded and
//    no longer displayed. On by default on the real Linux path;
//    ULTIMA_CAM_ZEROCOPY=0 forces everything through path 2.
//
// 2. CONVERTED QImages (SurroundView's stitched 360 view, and the only
//    path the macOS/simulated build has). A capture thread decodes UYVY ->
//    RGBA8888 at 1/kDecimation resolution (NEON) and hands the newest
//    frame to the GUI thread, which publishes it via currentFrame() /
//    frameReady(). Conversion only runs while a consumer is registered
//    (addFrameConsumer(), driven by SurroundView's visibility) — a drive
//    that never opens the 360 view never pays for it.
//
// All per-frame work happens on a dedicated capture thread (one per feed;
// they land on the A53's spare cores). The GUI thread only receives a
// ready QImage pointer-swap per frame; the profiling above measured the
// old on-GUI-thread design costing 10+% of that thread per camera, on the
// same thread every QML animation runs on.
//
// Lazily opened: the device is only opened while active is true, driven by
// the camera screens' open()/close(). This project measures first-Qt-frame
// to the millisecond (see NOTES.md's measure-boot.sh); opening a capture
// device and negotiating a streaming format at construction would regress
// that for a screen most drives never open.
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

    // Latest converted frame (path 2 above; null when nothing has asked
    // for conversion yet). Written ONLY on the GUI thread (onWorkerFrame
    // receives the capture thread's result via a queued invocation), read
    // by renderers during the scene-graph sync phase while the GUI thread
    // is blocked — that pairing is what makes the lock-free read safe. If
    // either side of that ever changes, this needs a QMutex — don't drop
    // one silently.
    QImage currentFrame() const { return m_frame; }

    // --- Converted-frame consumers (path 2) ---------------------------
    // SurroundView registers while visible (see surroundview.cpp); the
    // capture thread skips the UYVY->RGBA conversion entirely while the
    // count is zero and the zero-copy path is carrying the display.
    // Thread-safe (atomic).
    void addFrameConsumer() { m_frameConsumers.ref(); }
    void removeFrameConsumer() { m_frameConsumers.deref(); }

    // --- Zero-copy display path (path 1) ------------------------------
    // See dmabuftexture.h for the consuming side. All of these are
    // thread-safe; acquire/release run on the render thread.
    bool zeroCopy() const { return m_zeroCopy; }
    int captureWidth() const { return m_captureWidth; }
    int captureHeight() const { return m_captureHeight; }
    int captureStride() const { return m_bytesPerLine; }
    int dmabufFd(int index) const;
    // Stream identity: bumped every time the device is (re)opened or
    // closed. Buffer indices, fds, and EGLImages from an older session are
    // dead — DmaBufTextureSet uses this to know when to drop its textures.
    quint32 bufferSession() const;
    // Returns the newest buffer index if its generation is newer than
    // *generation (both outparams updated), else -1. Takes a reference on
    // the returned index; pair every success with releaseBuffer().
    int acquireLatestBuffer(quint32 *generation, quint32 *session);
    void releaseBuffer(int index, quint32 session);

signals:
    void activeChanged();
    void streamingChanged();
    void failedChanged();
    void formatChanged();
    void frameReady();

private slots:
    void tryOpen();
    // Queued back from the capture thread — see CameraCaptureThread in
    // camerafeed.cpp.
    void onWorkerFrame();
    void onWorkerError();
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
    // m_frame is decoded at on the converted path. NOT the driver-granted
    // capture size: see kDecimation in camerafeed.cpp. (The zero-copy path
    // displays the full captureWidth/captureHeight — QML only uses these
    // for aspect, which decimation preserves.)
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    // Raw driver-granted capture size/stride (VIDIOC_S_FMT's actual grant).
    int m_captureWidth = 0;
    int m_captureHeight = 0;
    int m_bytesPerLine = 0;  // driver-granted V4L2 stride — see camerafeed.cpp's tryOpen()
    QImage m_frame;

    int m_fd = -1;
    QTimer m_reconnectTimer;
    // Backoff state for m_reconnectTimer — see scheduleReconnect() in
    // camerafeed.cpp for why a permanently-failing feed can't stay on a
    // flat 1s retry.
    int m_reconnectIntervalMs = 1000;
    void scheduleReconnect();

    // Perf instrumentation, opt-in via ULTIMA_CAM_FPS_LOG — see the
    // capture thread's fps log in camerafeed.cpp.
    bool m_fpsLogEnabled = false;

    bool m_zeroCopy = false;
    QAtomicInt m_frameConsumers{0};
    friend class CameraCaptureThread;
    class CameraCaptureThread *m_captureThread = nullptr;

    // 6 buffers on the zero-copy path: 1 being DMA-written + 1 ready in
    // the driver + 1 pending in the mailbox + display/prev held by the
    // renderer + 1 margin. (4x cameras x 6 x 4MB = 96MB of the 128MB CMA
    // pool — see NOTES.md before raising this.) kMaxBuffers just bounds
    // the arrays.
    static constexpr int kMaxBuffers = 8;

    // Zero-copy buffer mailbox. m_bufMutex guards everything below it.
    // Lending rules live with publishBuffer()/acquireLatestBuffer()/
    // releaseBuffer() in camerafeed.cpp.
    mutable QMutex m_bufMutex;
    int m_dmabufFds[kMaxBuffers];
    int m_bufRefs[kMaxBuffers];
    int m_pendingIndex = -1;
    bool m_pendingAcquired = false;
    quint32 m_pendingGen = 0;
    quint32 m_bufSession = 0;
    QVector<int> m_retired;
    void publishBuffer(int index);        // capture thread only
    QVector<int> takeRetiredBuffers();    // capture thread only
    void resetBufferMailbox();

#ifdef __linux__
    // mmap'd V4L2 capture buffers (VIDIOC_REQBUFS/QUERYBUF/QBUF/DQBUF).
    struct MappedBuffer { void *start = nullptr; size_t length = 0; };
    MappedBuffer m_buffers[kMaxBuffers];
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
