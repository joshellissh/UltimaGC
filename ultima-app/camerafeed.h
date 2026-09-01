#ifndef CAMERAFEED_H
#define CAMERAFEED_H

#include <QObject>
#include <QString>
#include <QImage>

// Placeholder camera feed. No camera driver is wired up yet (see
// beagley-ai/NOTES.md for the history) — every feed just shows a placeholder
// image wherever a camera screen asks for one, instead of attempting to open
// real capture hardware. ULTIMA_CAM_IMAGE_DIR (env var), if set, serves a
// real photo per feed instead (<dir>/<label>.png, e.g. "cam1.png") — useful
// for previewing real camera-mount photos ahead of a real driver existing;
// unset, missing, or undecodable all fall back to a plain drawn card.
//
// The zero-copy accessors below (zeroCopy()/captureWidth()/dmabufFd()/etc.)
// are inert stubs kept only so CameraView/SurroundView/DmaBufTextureSet
// don't need to change: zeroCopy() always reports false, which routes every
// consumer through the QImage path (currentFrame()) they already fall back
// to. Re-wire these for real once a driver exposes real V4L2 capture nodes.
class CameraFeed : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY failedChanged)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY formatChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY formatChanged)

public:
    explicit CameraFeed(const QString &label = QStringLiteral("cam1"), QObject *parent = nullptr);
    ~CameraFeed();

    bool active() const { return m_active; }
    void setActive(bool on);

    bool streaming() const { return m_streaming; }
    bool failed() const { return m_failed; }
    int frameWidth() const { return m_frameWidth; }
    int frameHeight() const { return m_frameHeight; }

    // The placeholder image (path 2's only path now). Written only on the
    // GUI thread, same as before.
    QImage currentFrame() const { return m_frame; }

    // No real decode work happens per-frame anymore, so these are no-ops —
    // kept because SurroundView calls them to gate its (now nonexistent)
    // conversion cost.
    void addFrameConsumer() {}
    void removeFrameConsumer() {}

    // --- Inert zero-copy stubs — see the class comment. ------------------
    bool zeroCopy() const { return false; }
    int captureWidth() const { return 0; }
    int captureHeight() const { return 0; }
    int captureStride() const { return 0; }
    int dmabufFd(int) const { return -1; }
    quint32 bufferSession() const { return 0; }
    int acquireLatestBuffer(quint32 *, quint32 *) { return -1; }
    void releaseBuffer(int, quint32) {}

signals:
    void activeChanged();
    void streamingChanged();
    void failedChanged();
    void formatChanged();
    void frameReady();

private:
    void setStreaming(bool on);
    void setFailed(bool on);
    void showPlaceholder();

    QString m_label;
    bool m_active = false;
    bool m_streaming = false;
    bool m_failed = false;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    QImage m_frame;

    QImage m_placeholder;
    bool m_placeholderLoaded = false;
};

#endif
