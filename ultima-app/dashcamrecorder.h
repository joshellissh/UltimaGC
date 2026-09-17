#ifndef DASHCAMRECORDER_H
#define DASHCAMRECORDER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QString>
#include <QProcess>

class CameraFeed;

// Owns the dashcam recording policy: watches for the auto-mounted USB drive
// (/mnt/dvr, mounted by recipes-ultima/ultima-dvr-mount) and enables/disables
// continuous hardware-H.264 recording on every camera feed accordingly. The
// per-feed encoding + segment writing happen on each feed's capture thread —
// this class only decides when to record and where to. See DASHCAM.md.
//
// It also drives the "format this drive?" flow: it detects a USB disk that is
// plugged in but NOT set up for the DVR (no ULTIMA_DVR label), exposes it to
// QML (unformattedDrive / formatPromptOpen), and — on the user's confirmation
// (formatDrive()) — runs the /usr/bin/ultima-dvr-format helper (whole-disk
// exFAT labeled ULTIMA_DVR + udev re-trigger) and reports success/failure.
//
// Cross-platform: on the macOS/simulated dev build the drive is never "ready"
// and detection is inert (there is no Wave5 encoder there anyway); formatDrive()
// simulates a short success so the dialog flow can still be exercised.
class DashcamRecorder : public QObject
{
    Q_OBJECT
    // True while the DVR drive is mounted+writable and the feeds have been told
    // to record — the recording *policy* state, not "the encoder is emitting
    // bytes this instant" (each feed's Wave5 encoder is created lazily on its
    // first frame). Exposed so the gauge cluster can show a "not recording"
    // indicator (see main.qml).
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    // A plugged-in USB disk that isn't set up for the DVR ("/dev/sdX"), or ""
    // when there is none. See detectFormatCandidate().
    Q_PROPERTY(QString unformattedDrive READ unformattedDrive NOTIFY unformattedDriveChanged)
    // Format-flow state machine for the dialog: idle | formatting | succeeded |
    // failed. "idle" with a candidate present is the initial "format this?"
    // prompt; the others are the in-progress / result screens.
    Q_PROPERTY(QString formatState READ formatState NOTIFY formatStateChanged)
    // Human-readable success/failure line shown in the dialog (the format
    // helper's own reason on failure).
    Q_PROPERTY(QString formatMessage READ formatMessage NOTIFY formatMessageChanged)
    // Single source of truth for whether the dialog should be on screen:
    // a fresh (non-dismissed) candidate, or any non-idle format state.
    Q_PROPERTY(bool formatPromptOpen READ formatPromptOpen NOTIFY formatPromptOpenChanged)
public:
    explicit DashcamRecorder(QVector<CameraFeed *> feeds,
                             QString root = QStringLiteral("/mnt/dvr/ULTIMA"),
                             int bitrateBps = 8000000, int gop = 25, int segSeconds = 60,
                             QObject *parent = nullptr);

    // Begin polling for the drive (and record whenever it is present).
    void start();

    bool isRecording() const { return m_recording; }
    QString unformattedDrive() const { return m_unformattedDrive; }
    QString formatState() const { return m_formatState; }
    QString formatMessage() const { return m_formatMessage; }
    bool formatPromptOpen() const;

    // Confirmed by the user (FORMAT button): wipe the detected drive to
    // exFAT/ULTIMA_DVR and wait for it to mount. Destructive; a no-op unless a
    // candidate is present and no format is already running.
    Q_INVOKABLE void formatDrive();
    // Dismiss the dialog (CLOSE/OK): clears a shown result and suppresses the
    // prompt for the current drive until it is re-plugged.
    Q_INVOKABLE void dismissFormatPrompt();
    // Debug/test hook (any platform): force the dialog into a given state
    // without a real drive/format, so every screen can be screenshotted. Pins
    // detection until dismissed. Driven by /tmp/ultima-dvrtest.request (main.cpp).
    Q_INVOKABLE void debugForceState(const QString &state,
                                     const QString &drive = QStringLiteral("debug"),
                                     const QString &message = QString());

signals:
    void recordingChanged();
    void unformattedDriveChanged();
    void formatStateChanged();
    void formatMessageChanged();
    void formatPromptOpenChanged();

private slots:
    void poll();
    void onFormatFinished(int exitCode, QProcess::ExitStatus status);
    void onFormatWaitTick();

private:
    // True when /mnt/dvr has a real filesystem mounted and is writable.
    bool dvrReady() const;

    // Format-candidate detection (Linux only; "" elsewhere).
    void updateUnformattedDrive();
    QString detectFormatCandidate() const;
    bool deviceHasMount(const QString &name) const;

    void setUnformattedDrive(const QString &dev);
    void setFormatState(const QString &s);
    void setFormatMessage(const QString &m);

    QVector<CameraFeed *> m_feeds;
    QString m_root;
    int m_bitrate;
    int m_gop;
    int m_segSeconds;
    QTimer m_timer;
    bool m_recording = false;

    QString m_unformattedDrive;                        // "/dev/sdX" or ""
    QString m_formatState = QStringLiteral("idle");    // idle|formatting|succeeded|failed
    QString m_formatMessage;
    bool m_promptDismissed = false;   // user closed the prompt for this drive
    bool m_debugOverride = false;     // a debug/test hook is pinning the dialog
    QString m_debCandidate;           // debounce: last raw candidate seen
    int m_debCount = 0;               // consecutive polls it has been seen
    QProcess *m_fmtProc = nullptr;    // the running ultima-dvr-format process
    QTimer m_formatWaitTimer;         // polls dvrReady() after mkfs until mount/timeout
    int m_formatWaitMs = 0;
};

#endif // DASHCAMRECORDER_H
