#ifndef CANBUS_H
#define CANBUS_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QSocketNotifier>

class OdoStore;

// Reads the Syvecs S7+ fixed datastream from a SocketCAN interface (can0 by
// default), decodes per the published DBC (S7 Fixed Stream v3), and exposes
// gauge channels as Qt properties. Drop-in replacement for SimEngine on the
// QML side.
//
// Speed integration drives the odometer through OdoStore.
class CanBus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)             // mph
    Q_PROPERTY(double rpm READ rpm NOTIFY rpmChanged)
    Q_PROPERTY(int gear READ gear NOTIFY gearChanged)                   // -2=P -1=R 0=N 1..8
    Q_PROPERTY(double fuelLevel READ fuelLevel NOTIFY fuelLevelChanged) // 0..1
    Q_PROPERTY(double coolantTemp READ coolantTemp NOTIFY coolantTempChanged) // °F
    Q_PROPERTY(double boost READ boost NOTIFY boostChanged)             // psi (clamped >= 0)
    Q_PROPERTY(double totalOdo READ totalOdo WRITE setTotalOdo NOTIFY totalOdoChanged)
    Q_PROPERTY(double tripOdo READ tripOdo WRITE setTripOdo NOTIFY tripOdoChanged)
    Q_PROPERTY(bool oilPressureWarn READ oilPressureWarn NOTIFY oilPressureWarnChanged)
    Q_PROPERTY(bool batteryWarn READ batteryWarn NOTIFY batteryWarnChanged)
    Q_PROPERTY(bool coolantWarn READ coolantWarn NOTIFY coolantWarnChanged)
    Q_PROPERTY(bool checkEngine READ checkEngine NOTIFY checkEngineChanged)
    // Channels not present in the Syvecs fixed stream — exposed for QML
    // compatibility, never set true by this class.
    Q_PROPERTY(bool leftIndicator MEMBER m_leftIndicator CONSTANT)
    Q_PROPERTY(bool rightIndicator MEMBER m_rightIndicator CONSTANT)
    Q_PROPERTY(bool lowBeams MEMBER m_lowBeams CONSTANT)
    Q_PROPERTY(bool highBeams MEMBER m_highBeams CONSTANT)

public:
    explicit CanBus(OdoStore *odo, const QString &iface = QStringLiteral("can0"),
                    QObject *parent = nullptr);
    ~CanBus();

    double speed() const { return m_speed; }
    double rpm() const { return m_rpm; }
    int gear() const { return m_gear; }
    double fuelLevel() const { return m_fuelLevel; }
    double coolantTemp() const { return m_coolantTempF; }
    double boost() const { return m_boostPsi; }
    double totalOdo() const { return m_totalOdo; }
    double tripOdo() const { return m_tripOdo; }
    bool oilPressureWarn() const { return m_oilPressureWarn; }
    bool batteryWarn() const { return m_batteryWarn; }
    bool coolantWarn() const { return m_coolantWarn; }
    bool checkEngine() const { return m_checkEngine; }

    void setTotalOdo(double v);
    void setTripOdo(double v);

public slots:
    // Flush in-memory odometer to OdoStore and persist.
    void save();

signals:
    void speedChanged();
    void rpmChanged();
    void gearChanged();
    void fuelLevelChanged();
    void coolantTempChanged();
    void boostChanged();
    void totalOdoChanged();
    void tripOdoChanged();
    void oilPressureWarnChanged();
    void batteryWarnChanged();
    void coolantWarnChanged();
    void checkEngineChanged();

private slots:
    void onReadable();
    void tryConnect();
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    void simulateTick();
#endif

private:
    void decodeFrame(quint32 id, const quint8 *data, int dlc);
    void accumulateOdometer();
    void closeSocket();

    OdoStore *m_odo;
    QString m_iface;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_reconnectTimer;

    // Gauge state
    double m_speed = 0.0;         // mph
    double m_rpm = 0.0;
    int m_gear = 0;               // -2=P -1=R 0=N 1..8
    double m_fuelLevel = 0.0;     // 0..1
    double m_coolantTempF = 0.0;
    double m_boostPsi = 0.0;
    double m_totalOdo = 0.0;
    double m_tripOdo = 0.0;
    bool m_oilPressureWarn = false;
    bool m_batteryWarn = false;
    bool m_coolantWarn = false;
    bool m_checkEngine = false;
    bool m_leftIndicator = false;
    bool m_rightIndicator = false;
    bool m_lowBeams = false;
    bool m_highBeams = false;

    // Odometer integration
    qint64 m_lastSpeedMs = 0;

#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    // Dev-build data simulator (macOS always, Linux dev builds when built
    // with CONFIG+=ultima_dev_sim — mirrors SimEngine.qml's phase-based
    // driving profile so gauges animate without real CAN hardware).
    QTimer m_simTimer;
    double m_simTargetSpeed = 0.0;   // mph
    double m_simPhaseTimer = 0.0;    // seconds remaining in current phase
    double m_simAccel = 0.0;         // mph per tick
#endif
};

#endif
