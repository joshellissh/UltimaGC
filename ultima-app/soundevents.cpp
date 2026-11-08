#include "soundevents.h"

#include <QDateTime>
#include <cstdio>

#include "canbus.h"
#include "soundplayer.h"

namespace {
constexpr qint64 kSettleMs = 8000;         // hold CAN-derived sounds back after launch
constexpr int kAlarmRepeatMs = 4000;       // oil/coolant alarm repeat while the fault persists
constexpr qint64 kAlarmEdgeCooldownMs = 2500;   // flicker guard around the thresholds
constexpr int kBatteryPollMs = 2000;
constexpr int kBatteryBadPolls = 2;        // engine running + still low for ~2-4 s
constexpr qint64 kBatteryRepeatMs = 60000;
constexpr qint64 kLowFuelCooldownMs = 120000;   // fuel sender slosh around 1/4 tank
constexpr int kTickMaxAgeMs = 400;         // a tick later than this is dropped, not played out of step with the icon
constexpr qint64 kMinTickGapMs = 150;      // any two ticks closer than this are one
// Lit/dark strictly alternate (350 ms each), so two ticks of the SAME polarity
// closer than this are a duplicate, not two beats. This is what merges the
// engage tick with the blink-clock tick when the signal comes on in a dark
// phase: the camera overlay popping up stalls the GUI thread ~170 ms between
// the two handlers, which a plain time window (150 ms) let through as a
// double tick that sounded like a rushed first tick/tock.
constexpr qint64 kSamePolarityGapMs = 600;
constexpr double kEngineRunningRpm = 600.0; // same threshold canbus.cpp uses for the oil warning
} // namespace

SoundEvents::SoundEvents(SoundPlayer *player, CanBus *can, QObject *window, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_can(can)
    , m_window(window)
{
    m_uptime.start();

    m_log = qEnvironmentVariableIntValue("ULTIMA_SOUND_LOG") != 0;
    if (m_log) {
        m_heartbeat.setInterval(10);
        m_hbLastMs = QDateTime::currentMSecsSinceEpoch();
        connect(&m_heartbeat, &QTimer::timeout, this, [this] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 late = now - m_hbLastMs - 10;
            m_hbLastMs = now;
            if (late > 40)
                fprintf(stderr, "[snd-t] t=%lld GUI-thread stall %lld ms\n",
                        qint64(now % 1000000), late);
        });
        m_heartbeat.start();
    }

    // --- Warnings -----------------------------------------------------
    connect(can, &CanBus::oilPressureWarnChanged, this, [this] {
        if (m_can->oilPressureWarn())
            fire(QStringLiteral("alarm"), QStringLiteral("alarm"), kAlarmEdgeCooldownMs);
    });
    connect(can, &CanBus::coolantWarnChanged, this, [this] {
        if (m_can->coolantWarn())
            fire(QStringLiteral("alarm"), QStringLiteral("alarm"), kAlarmEdgeCooldownMs);
    });
    // Repeat while either is still active. Shares the "alarm" cooldown key
    // with the edge path above, so a fresh edge isn't doubled by the timer.
    m_alarmRepeat.setInterval(kAlarmRepeatMs);
    connect(&m_alarmRepeat, &QTimer::timeout, this, &SoundEvents::repeatAlarms);
    m_alarmRepeat.start();

    connect(can, &CanBus::lowFuelWarnChanged, this, [this] {
        if (m_can->lowFuelWarn())
            fire(QStringLiteral("fuel"), QStringLiteral("warn"), kLowFuelCooldownMs);
    });
    connect(can, &CanBus::checkEngineChanged, this, [this] {
        if (m_can->checkEngine())
            fire(QStringLiteral("checkengine"), QStringLiteral("warn"), 10000);
    });
    connect(can, &CanBus::absWarnChanged, this, [this] {
        if (m_can->absWarn())
            fire(QStringLiteral("abs"), QStringLiteral("warn"), 10000);
    });

    m_batteryPoll.setInterval(kBatteryPollMs);
    connect(&m_batteryPoll, &QTimer::timeout, this, &SoundEvents::pollBattery);
    connect(can, &CanBus::batteryWarnChanged, this, &SoundEvents::onBatteryWarnChanged);

    // --- Turn signals / hazards ---------------------------------------
    connect(can, &CanBus::leftIndicatorChanged, this, &SoundEvents::onIndicatorsChanged);
    connect(can, &CanBus::rightIndicatorChanged, this, &SoundEvents::onIndicatorsChanged);
    connect(can, &CanBus::hazardChanged, this, &SoundEvents::onIndicatorsChanged);
    if (window)
        connect(window, SIGNAL(blinkPhaseChanged()), this, SLOT(onBlinkPhase()));
}

bool SoundEvents::settled() const
{
    return m_uptime.elapsed() >= kSettleMs;
}

bool SoundEvents::indicating() const
{
    return m_can->leftIndicator() || m_can->rightIndicator() || m_can->hazard();
}

void SoundEvents::fire(const QString &event, const QString &sound, qint64 cooldownMs, bool canEvent)
{
    if (canEvent && !settled())
        return;
    const qint64 now = m_uptime.elapsed();
    const auto it = m_lastFiredMs.constFind(event);
    if (cooldownMs > 0 && it != m_lastFiredMs.constEnd() && now - it.value() < cooldownMs)
        return;
    m_lastFiredMs.insert(event, now);
    m_player->play(sound);
}

void SoundEvents::repeatAlarms()
{
    if (m_can->oilPressureWarn() || m_can->coolantWarn())
        fire(QStringLiteral("alarm"), QStringLiteral("alarm"), kAlarmRepeatMs - 500);
}

void SoundEvents::onBatteryWarnChanged()
{
    m_battBadPolls = 0;
    if (m_can->batteryWarn())
        m_batteryPoll.start();
    else
        m_batteryPoll.stop();
}

// batteryWarn is vbat < 12.5 V, which is simply true at key-on and while
// cranking. Only a low battery with the engine running (alternator should be
// charging) for a couple of polls is worth a sound.
void SoundEvents::pollBattery()
{
    if (m_can->batteryWarn() && m_can->rpm() >= kEngineRunningRpm)
        ++m_battBadPolls;
    else
        m_battBadPolls = 0;
    if (m_battBadPolls >= kBatteryBadPolls)
        fire(QStringLiteral("battery"), QStringLiteral("warn"), kBatteryRepeatMs);
}

void SoundEvents::tick(bool lit, const char *src)
{
    const qint64 now = m_uptime.elapsed();
    const qint64 sinceLast = now - m_lastTickMs;
    const bool merged = sinceLast < kMinTickGapMs ||
                        (lit == m_lastTickLit && sinceLast < kSamePolarityGapMs);
    if (m_log)
        fprintf(stderr, "[snd-t] t=%lld %s %s since-last=%lld ms%s\n",
                qint64(QDateTime::currentMSecsSinceEpoch() % 1000000),
                lit ? "tick" : "tock", src, qint64(now - m_lastTickMs),
                merged ? "  (merged/suppressed)" : "");
    if (merged)
        return;
    m_lastTickMs = now;
    m_lastTickLit = lit;
    m_player->play(lit ? QStringLiteral("tick") : QStringLiteral("tock"), kTickMaxAgeMs);
}

// Nothing was signalling and now something is: tick straight away. The QML
// blink clock only emits blinkPhaseChanged when the phase actually flips —
// if it was already "lit" when the signal engaged it says nothing until the
// first 350 ms toggle — so without this the first tick would be missing.
// Only the none -> some transition ticks: a change while a signal is already
// blinking (hazard on top of left, one of two released) would otherwise play
// an off-beat tick mid-phase.
void SoundEvents::onIndicatorsChanged()
{
    const bool now = indicating();
    const bool engaged = now && !m_prevIndicating;
    m_prevIndicating = now;
    if (engaged)
        tick(true, "engage");
}

void SoundEvents::onBlinkPhase()
{
    if (!indicating())
        return;
    // The blink clock can flip dark -> lit as part of the same engage, and its
    // handler runs BEFORE onIndicatorsChanged (QML's bindings connect first).
    // Between the two the camera overlay popping up stalls the GUI thread
    // ~170 ms, and the blink Timer only really starts after that stall — so
    // the engage handler's tick, not this early one, is the one in step with
    // the 350 ms rhythm and the icon. Leave the first tick to it.
    if (!m_prevIndicating) {
        if (m_log)
            fprintf(stderr, "[snd-t] t=%lld lit phase flip deferred to the engage handler\n",
                    qint64(QDateTime::currentMSecsSinceEpoch() % 1000000));
        return;
    }
    tick(m_window->property("blinkPhase").toBool(), "phase");
}
