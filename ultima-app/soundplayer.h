#ifndef SOUNDPLAYER_H
#define SOUNDPLAYER_H

#include <QElapsedTimer>
#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QWaitCondition>
#include <thread>

// Plays the dash's short alert/feedback sounds (warning alarms,
// turn-signal ticks) out of the HDMI audio path.
//
// Each sound name ("alarm", "tick", ...) is a WAV file, <name>.wav, looked up in
// $ULTIMA_SOUND_DIR, /data/sounds (writable: swap a sound without a rebuild),
// then /usr/share/ultima/sounds (shipped in the image from
// ultima-app/assets/sounds/). Any ordinary WAV works — it is converted to the
// stream format on first use. A name with no usable file falls back to a
// built-in synthesised tone (soundplayer.cpp), so a missing file is never
// silent. Playback goes straight to ALSA (libasound, already in the image) from a
// dedicated worker thread, so play() is cheap and safe to call from the GUI
// thread, the CAN decode path, or QML, and a stalled audio device can never
// hold up a frame.
//
// HDMI specifics the worker handles (BeagleY-AI: McASP1 -> IT66122 -> panel,
// S32_LE stereo, 32/48 kHz only — see beagley-ai/NOTES.md "HDMI audio"):
//  - The sink mutes/relocks when the audio stream starts, clipping the first
//    ~100-250 ms. A freshly opened stream therefore gets a short silent
//    lead-in, and the stream is then kept alive by feeding silence, so a
//    burst of events (turn-signal ticks, a repeating alarm) is never clipped.
//    It is never closed on idle: closing this card takes 0.5-5 s (HW_FREE +
//    close) and, on the worker thread, held up the next tick that long —
//    the backlog then played as a rapid burst.
//  - Starting a stream on a cold card takes ~2 s (snd_pcm_prepare), so the
//    worker opens it in the background as soon as the card exists (retrying
//    every 2 s, quietly) rather than on the first sound. If a sound arrives
//    before the card is up (audio modules load at udev coldplug, after the
//    first frame) it is dropped, not queued.
//
// Compiled for real only when ULTIMA_ALSA is defined (qmake CONFIG+=
// ultima_alsa, set by the Yocto recipe); everywhere else (macOS / WSL dev
// builds) play() is a no-op, so the rest of the app needs no #ifdefs.
//
// Environment (all optional):
//   ULTIMA_SOUND=0              disable all sound
//   ULTIMA_SOUND_VOLUME=0..1    software gain (default 0.6; the HDMI card has
//                               no hardware mixer)
//   ULTIMA_SOUND_DIR=path       extra directory searched first for <name>.wav
//   ULTIMA_SOUND_DEVICE=name    ALSA PCM (default "default", which converts
//                               format/rate to what the McASP accepts)
class SoundPlayer : public QObject
{
    Q_OBJECT
public:
    explicit SoundPlayer(QObject *parent = nullptr);
    ~SoundPlayer() override;

    // Queue the named sound. Unknown names and a disabled player are ignored;
    // at most a few sounds wait behind the one playing, extras are dropped
    // (a stale tick is worse than no tick). maxAgeMs >= 0 additionally drops
    // the request if the worker has not reached it within that many ms —
    // for timing-critical sounds (turn-signal ticks) that would otherwise
    // play late and bunch up behind a stalled device. -1 = never stale.
    Q_INVOKABLE void play(const QString &name, int maxAgeMs = -1);

    // Every sound name play() accepts (for the test hook / docs).
    static QStringList names();

private:
    void run();

    bool m_enabled = true;
    float m_volume = 0.6f;
    bool m_log = false;             // ULTIMA_SOUND_LOG=1: "[snd-t]" timing lines
    QString m_device;

    QMutex m_mutex;
    QWaitCondition m_cond;
    struct Request {
        QString name;
        int maxAgeMs = -1;
        QElapsedTimer age;
    };
    QQueue<Request> m_queue;
    bool m_stop = false;
    std::thread m_thread;
};

#endif // SOUNDPLAYER_H
