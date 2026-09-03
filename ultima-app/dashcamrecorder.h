#ifndef DASHCAMRECORDER_H
#define DASHCAMRECORDER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QString>

class CameraFeed;

// Owns the dashcam recording policy: watches for the auto-mounted USB drive
// (/mnt/dvr, mounted by recipes-ultima/ultima-dvr-mount) and enables/disables
// continuous hardware-H.264 recording on every camera feed accordingly. The
// per-feed encoding + segment writing happen on each feed's capture thread —
// this class only decides when to record and where to. See DASHCAM.md.
//
// Cross-platform: on the macOS/simulated dev build the drive is never "ready",
// so this stays inert (there is no Wave5 encoder there anyway).
class DashcamRecorder : public QObject
{
    Q_OBJECT
public:
    explicit DashcamRecorder(QVector<CameraFeed *> feeds,
                             QString root = QStringLiteral("/mnt/dvr/ULTIMA"),
                             int bitrateBps = 8000000, int gop = 25, int segSeconds = 60,
                             QObject *parent = nullptr);

    // Begin polling for the drive (and record whenever it is present).
    void start();

private slots:
    void poll();

private:
    // True when /mnt/dvr has a real filesystem mounted and is writable.
    bool dvrReady() const;

    QVector<CameraFeed *> m_feeds;
    QString m_root;
    int m_bitrate;
    int m_gop;
    int m_segSeconds;
    QTimer m_timer;
    bool m_recording = false;
};

#endif // DASHCAMRECORDER_H
