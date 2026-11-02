#ifndef SOUNDEVENTS_H
#define SOUNDEVENTS_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class CanBus;
class DashcamRecorder;
class SoundPlayer;

// Decides *when* the dash makes a sound; SoundPlayer decides how. Pure signal
// wiring over existing objects — CanBus, DashcamRecorder and main.qml's window
// are observed, never modified — so a sound policy change lives here only.
//
//   warnings    oil pressure / coolant: "alarm" on the rising edge, repeated
//               every kAlarmRepeatMs while the fault persists. Battery: "warn"
//               if still low 3 s after the engine is running (the voltage dips
//               below 12.5 V at key-on and while cranking, which is not a
//               fault). Low fuel (cooldown: the sender sloshes around the
//               threshold), check engine, ABS: "warn" on the rising edge.
//   drive mode  beep count = mode (see the mode_* sounds), on change.
//   turn signal tick/tock off the QML blink clock (main.qml `blinkPhase`), so
//               the sound is in step with the lit icon by construction.
//   dashcam     recording started / stopped, "format this drive?" prompt,
//               format succeeded / failed.
//
// CAN-derived sounds are held back for kSettleMs after launch: the app starts
// with every flag false and the first CAN frames then flip several to their
// real value, which is state catching up, not an event.
class SoundEvents : public QObject
{
    Q_OBJECT
public:
    // `window` is main.qml's root object (for blinkPhase); may be null.
    SoundEvents(SoundPlayer *player, CanBus *can, DashcamRecorder *dashcam,
                QObject *window, QObject *parent = nullptr);

private slots:
    void onBlinkPhase();
    void onIndicatorsChanged();
    void onDriveModeChanged();
    void onBatteryWarnChanged();
    void pollBattery();
    void repeatAlarms();

private:
    // Play `sound` for `event` unless that event already fired within
    // cooldownMs. canEvent sounds are dropped silently while CAN state is
    // still settling after launch — see the class comment.
    void fire(const QString &event, const QString &sound, qint64 cooldownMs = 0, bool canEvent = true);
    void tick(bool lit);
    bool settled() const;
    bool indicating() const;

    SoundPlayer *m_player;
    CanBus *m_can;
    QObject *m_window;
    QElapsedTimer m_uptime;
    QHash<QString, qint64> m_lastFiredMs;
    QTimer m_alarmRepeat;
    QTimer m_batteryPoll;
    int m_battBadPolls = 0;
    qint64 m_lastTickMs = -1000;
};

#endif // SOUNDEVENTS_H
