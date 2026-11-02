#include "soundevents.h"

#include "canbus.h"
#include "dashcamrecorder.h"
#include "soundplayer.h"

namespace {
constexpr qint64 kSettleMs = 8000;         // hold CAN-derived sounds back after launch
constexpr int kAlarmRepeatMs = 4000;       // oil/coolant alarm repeat while the fault persists
constexpr qint64 kAlarmEdgeCooldownMs = 2500;   // flicker guard around the thresholds
constexpr int kBatteryPollMs = 2000;
constexpr int kBatteryBadPolls = 2;        // engine running + still low for ~2-4 s
constexpr qint64 kBatteryRepeatMs = 60000;
constexpr qint64 kLowFuelCooldownMs = 120000;   // fuel sender slosh around 1/4 tank
constexpr qint64 kMinTickGapMs = 150;      // merge the engage tick with the first blink-phase tick
constexpr double kEngineRunningRpm = 600.0; // same threshold canbus.cpp uses for the oil warning

int driveModeLevel(const QString &mode)
{
    if (mode == QLatin1String("VALET"))  return 0;
    if (mode == QLatin1String("SPORT"))  return 1;
    if (mode == QLatin1String("SPORT+")) return 2;
    if (mode == QLatin1String("RACE"))   return 3;
    return -1;   // blank / unknown calSelect: say nothing
}
} // namespace

SoundEvents::SoundEvents(SoundPlayer *player, CanBus *can, DashcamRecorder *dashcam,
                         QObject *window, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_can(can)
    , m_window(window)
{
    m_uptime.start();

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

    // --- Drive mode ---------------------------------------------------
    connect(can, &CanBus::driveModeChanged, this, &SoundEvents::onDriveModeChanged);

    // --- Turn signals / hazards ---------------------------------------
    connect(can, &CanBus::leftIndicatorChanged, this, &SoundEvents::onIndicatorsChanged);
    connect(can, &CanBus::rightIndicatorChanged, this, &SoundEvents::onIndicatorsChanged);
    connect(can, &CanBus::hazardChanged, this, &SoundEvents::onIndicatorsChanged);
    if (window)
        connect(window, SIGNAL(blinkPhaseChanged()), this, SLOT(onBlinkPhase()));

    // --- Dashcam ------------------------------------------------------
    connect(dashcam, &DashcamRecorder::recordingChanged, this, [this, dashcam] {
        fire(QStringLiteral("rec"),
             dashcam->isRecording() ? QStringLiteral("rec_start") : QStringLiteral("rec_stop"),
             3000, false);
    });
    connect(dashcam, &DashcamRecorder::formatPromptOpenChanged, this, [this, dashcam] {
        if (dashcam->formatPromptOpen() && dashcam->formatState() == QLatin1String("idle"))
            fire(QStringLiteral("prompt"), QStringLiteral("prompt"), 2000, false);
    });
    connect(dashcam, &DashcamRecorder::formatStateChanged, this, [this, dashcam] {
        const QString s = dashcam->formatState();
        if (s == QLatin1String("succeeded"))
            fire(QStringLiteral("format"), QStringLiteral("success"), 0, false);
        else if (s == QLatin1String("failed"))
            fire(QStringLiteral("format"), QStringLiteral("error"), 0, false);
    });
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

void SoundEvents::onDriveModeChanged()
{
    static const char *const kSounds[] = { "mode_valet", "mode_sport", "mode_sportplus", "mode_race" };
    const int level = driveModeLevel(m_can->driveMode());
    if (level < 0)
        return;
    fire(QStringLiteral("mode"), QLatin1String(kSounds[level]), 300);
}

void SoundEvents::tick(bool lit)
{
    const qint64 now = m_uptime.elapsed();
    if (now - m_lastTickMs < kMinTickGapMs)
        return;
    m_lastTickMs = now;
    m_player->play(lit ? QStringLiteral("tick") : QStringLiteral("tock"));
}

// A signal just engaged (or one of several changed): tick straight away. The
// QML blink clock only emits blinkPhaseChanged when the phase actually flips
// — if it was already "lit" when the signal engaged it says nothing until the
// first 350 ms toggle — so without this the first tick would be missing.
void SoundEvents::onIndicatorsChanged()
{
    if (indicating())
        tick(true);
}

void SoundEvents::onBlinkPhase()
{
    if (!indicating())
        return;
    tick(m_window->property("blinkPhase").toBool());
}
