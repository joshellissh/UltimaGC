#ifndef CANBUS_H
#define CANBUS_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QSocketNotifier>
#include <QVariantMap>

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
    // compatibility. On real hardware these are now decoded from the MCE18
    // CAN expander's DIN0-7 bitmask (see decodeFrame()'s 0x702 case), not
    // the ECU; the dev-build simulator (see simulateTick()) still drives
    // lowBeams/highBeams/cruiseControl for layout testing.
    Q_PROPERTY(bool leftIndicator READ leftIndicator NOTIFY leftIndicatorChanged)
    Q_PROPERTY(bool rightIndicator READ rightIndicator NOTIFY rightIndicatorChanged)
    Q_PROPERTY(bool lowBeams READ lowBeams NOTIFY lowBeamsChanged)
    Q_PROPERTY(bool highBeams READ highBeams NOTIFY highBeamsChanged)
    Q_PROPERTY(bool axleLift READ axleLift NOTIFY axleLiftChanged)
    Q_PROPERTY(bool cruiseControl READ cruiseControl NOTIFY cruiseControlChanged)
    // Automatic/manual shift mode — not on the Syvecs fixed stream. Defaults
    // to Automatic on real hardware; on real hardware now driven by the
    // MCE18's DIN7 (see decodeFrame()'s 0x702 case — DIN7 asserted = Manual,
    // so an unwired/unasserted input still reads Automatic, preserving this
    // default); the dev-build simulator (see simulateTick()) toggles it for
    // layout review.
    Q_PROPERTY(bool transmissionAuto READ transmissionAuto NOTIFY transmissionAutoChanged)
    // Drive mode selector — not on the Syvecs fixed stream. One of "SPORT",
    // "SPORT+", "RACE"; the dev-build simulator (see simulateTick()) cycles
    // through them for layout review.
    Q_PROPERTY(QString driveMode READ driveMode NOTIFY driveModeChanged)
    // Diagnostics screen data — the ~40 CAN2 channels documented in the Auto
    // Bionics mapping sheet that aren't decoded by decodeFrame() yet (lambda,
    // per-cylinder knock retard, fuel/ignition trims, traction control,
    // torque estimates, a set of unconfirmed transmission channels). Bundled
    // as one map instead of ~40 individual Q_PROPERTYs on purpose: none of
    // these frame/offset/scaling triples have been re-verified against a
    // current SCal Datastreams screenshot the way the seven real properties
    // above were (see the mapMax/boost lesson in decodeFrame()), so this
    // project isn't ready to commit them as permanent typed API. Keys match
    // the sheet's own channel names. initDiag() seeds every key with a
    // resting default so QML bindings never see an undefined value — but
    // real hardware never advances past that seed (decodeFrame() doesn't
    // touch m_diag), so QML must gate display on diagLive below rather than
    // trust diag's values directly. Otherwise a real dash would show
    // plausible-looking resting numbers (1.00 λ, IDLE, ...) as if they were
    // live — the same failure shape as the mapMax boost gauge reading 0 as
    // if it meant something.
    Q_PROPERTY(QVariantMap diag READ diag NOTIFY diagChanged)
    // True only once the dev-build simulator has actually ticked at least
    // once. Stays false forever on real hardware until real decoding is
    // added for these channels — QML should show "--" rather than diag's
    // seeded defaults when this is false.
    Q_PROPERTY(bool diagLive READ diagLive NOTIFY diagLiveChanged)

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
    bool leftIndicator() const { return m_leftIndicator; }
    bool rightIndicator() const { return m_rightIndicator; }
    bool lowBeams() const { return m_lowBeams; }
    bool highBeams() const { return m_highBeams; }
    bool axleLift() const { return m_axleLift; }
    bool cruiseControl() const { return m_cruiseControl; }
    bool transmissionAuto() const { return m_transmissionAuto; }
    QString driveMode() const { return m_driveMode; }
    QVariantMap diag() const { return m_diag; }
    bool diagLive() const { return m_diagLive; }

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
    void leftIndicatorChanged();
    void rightIndicatorChanged();
    void lowBeamsChanged();
    void highBeamsChanged();
    void axleLiftChanged();
    void cruiseControlChanged();
    void transmissionAutoChanged();
    void driveModeChanged();
    void diagChanged();
    void diagLiveChanged();

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
    void initDiag();

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
    bool m_axleLift = false;
    bool m_cruiseControl = false;
    bool m_transmissionAuto = true;
    QString m_driveMode = QStringLiteral("SPORT");
    QVariantMap m_diag;   // diagnostics-screen channels — see Q_PROPERTY comment above
    bool m_diagLive = false;

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
    double m_simElapsedS = 0.0;      // free-running clock for sweep/toggle phases
    double m_simGearCycleTimer = 0.0; // seconds remaining before advancing the gear cycle
    int m_simGearCycleIndex = 0;      // index into the R/N/P/1..7 cycle
    double m_simKnock[6] = { 0, 0, 0, 0, 0, 0 }; // per-cylinder knock retard, deg — decays each tick
    double m_simKnockEventTimer = 0.0; // seconds until the next randomized knock blip
#endif
};

#endif
