#ifndef SYSTEMSTATS_H
#define SYSTEMSTATS_H

#include <QObject>
#include <QString>
#include <QTimer>

// Board/system stats that aren't sourced from CAN — currently just the SoC
// die temperature — polled and exposed as Qt properties for the Diagnostics
// screen (see DiagnosticScreen.qml's `source: "sysStats"` channels). Kept
// separate from CanBus, which is strictly the CAN2/ECU decoder.
class SystemStats : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double cpuTempC READ cpuTempC NOTIFY cpuTempCChanged)

public:
    explicit SystemStats(QObject *parent = nullptr);

    double cpuTempC() const { return m_cpuTempC; }

signals:
    void cpuTempCChanged();

private slots:
    void tick();

private:
    void setCpuTempC(double v);
#ifdef __linux__
    // Picks the sysfs thermal zone to poll — see systemstats.cpp for why
    // this isn't just a hardcoded thermal_zone0.
    QString resolveTempZonePath() const;
    QString m_tempZonePath;
#endif

    QTimer m_pollTimer;
    double m_cpuTempC = 0.0;
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    double m_simPhase = 0.0;
#endif
};

#endif
