#include "systemstats.h"

#ifdef __linux__
#include <QDir>
#include <QFile>
#include <QIODevice>
#endif
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
#include <cmath>
#endif

#include <cstdio>

namespace {
constexpr int kPollIntervalMs = 2000;
}

SystemStats::SystemStats(QObject *parent) : QObject(parent)
{
#ifdef __linux__
    m_tempZonePath = resolveTempZonePath();
#endif
    connect(&m_pollTimer, &QTimer::timeout, this, &SystemStats::tick);
    tick();
    m_pollTimer.start(kPollIntervalMs);
}

void SystemStats::setCpuTempC(double v)
{
    m_cpuTempC = v;
    emit cpuTempCChanged();
}

#ifdef __linux__
// The kernel numbers thermal zones by registration order, which varies by
// SoC/driver and isn't guaranteed to put the CPU/SoC sensor at zone0 — this
// repo has no local copy of the J722S devicetree to check (it's fetched by
// the TI SDK build, not vendored — see beagley-ai/NOTES.md), so scan every
// zone's `type` file for something CPU-ish rather than assume. Falls back
// to zone0 (logged) if nothing matches. Confirm on target with
// `cat /sys/class/thermal/thermal_zone*/type` and cross-check the reading
// against NOTES.md's "idles at 71-73 °C" boot-time baseline.
QString SystemStats::resolveTempZonePath() const
{
    QDir thermalDir(QStringLiteral("/sys/class/thermal"));
    const QStringList zones = thermalDir.entryList(
        QStringList() << QStringLiteral("thermal_zone*"), QDir::Dirs, QDir::Name);

    for (const QString &zone : zones) {
        QFile typeFile(thermalDir.filePath(zone) + QStringLiteral("/type"));
        if (!typeFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString type = QString::fromUtf8(typeFile.readAll()).trimmed().toLower();
        if (type.contains(QStringLiteral("cpu")) || type.contains(QStringLiteral("mpu"))
            || type.contains(QStringLiteral("main"))) {
            fprintf(stderr, "[sysstats] using %s (type=%s) for cpuTempC\n",
                    qPrintable(zone), qPrintable(type));
            return thermalDir.filePath(zone) + QStringLiteral("/temp");
        }
    }

    fprintf(stderr, "[sysstats] no cpu/mpu/main thermal zone matched — "
                     "falling back to thermal_zone0 for cpuTempC\n");
    return QStringLiteral("/sys/class/thermal/thermal_zone0/temp");
}
#endif

void SystemStats::tick()
{
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
    QFile f(m_tempZonePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return; // leave last-known value in place
    bool ok = false;
    const double milliC = f.readAll().trimmed().toDouble(&ok);
    if (ok)
        setCpuTempC(milliC / 1000.0);
#else
    // Slow drift around a plausible idle SoC temperature, purely for
    // Diagnostics-screen layout review on a dev build — see NOTES.md's
    // "idles at 71-73 °C" boot-time note for the real-hardware baseline.
    m_simPhase += 0.05;
    setCpuTempC(72.0 + 3.0 * std::sin(m_simPhase));
#endif
}
