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
    // Index into the dash's "PRN1234567" gear-position string, not a raw
    // ratio: 0=P 1=R 2=N 3..9=1st..7th. Chosen so main.qml's gear indicator
    // is a plain string index (see its comment), not an if/else chain.
    Q_PROPERTY(int gear READ gear NOTIFY gearChanged)
    Q_PROPERTY(double fuelLevel READ fuelLevel NOTIFY fuelLevelChanged) // 0..1
    Q_PROPERTY(double coolantTemp READ coolantTemp NOTIFY coolantTempChanged) // °F
    Q_PROPERTY(double boost READ boost NOTIFY boostChanged)             // psi (clamped >= 0)
    Q_PROPERTY(double totalOdo READ totalOdo WRITE setTotalOdo NOTIFY totalOdoChanged)
    Q_PROPERTY(double tripOdo READ tripOdo WRITE setTripOdo NOTIFY tripOdoChanged)
    Q_PROPERTY(bool oilPressureWarn READ oilPressureWarn NOTIFY oilPressureWarnChanged)
    Q_PROPERTY(bool batteryWarn READ batteryWarn NOTIFY batteryWarnChanged)
    Q_PROPERTY(bool coolantWarn READ coolantWarn NOTIFY coolantWarnChanged)
    Q_PROPERTY(bool lowFuelWarn READ lowFuelWarn NOTIFY lowFuelWarnChanged)
    Q_PROPERTY(bool checkEngine READ checkEngine NOTIFY checkEngineChanged)
    // Raw readings behind the four warn/status booleans above — the dash
    // only needs the boolean, but the diagnostics screen shows the actual
    // decoded value (see decodeFrame()'s 0x601/0x604/0x608/0x60E cases).
    Q_PROPERTY(double oilPressure READ oilPressure NOTIFY oilPressureChanged)     // psi, backs oilPressureWarn
    Q_PROPERTY(double vbat READ vbat NOTIFY vbatChanged)                         // V, backs batteryWarn
    Q_PROPERTY(QString cruiseState READ cruiseState NOTIFY cruiseStateChanged)   // OFF/ON/ACTIVE, backs cruiseControl
    Q_PROPERTY(int limpMode READ limpMode NOTIFY limpModeChanged)                // raw code, backs checkEngine
    // Channels not present in the Syvecs fixed stream — exposed for QML
    // compatibility. On real hardware these are decoded from the MCE18
    // CAN expander's DIN0-7 bitmask (see decodeFrame()'s 0x702 case), not
    // the ECU; the dev-build simulator (see simulateTick()) still drives
    // lowBeams for layout testing.
    Q_PROPERTY(bool leftIndicator READ leftIndicator NOTIFY leftIndicatorChanged)
    Q_PROPERTY(bool rightIndicator READ rightIndicator NOTIFY rightIndicatorChanged)
    Q_PROPERTY(bool lowBeams READ lowBeams NOTIFY lowBeamsChanged)
    Q_PROPERTY(bool highBeams READ highBeams NOTIFY highBeamsChanged)
    Q_PROPERTY(bool axleLift READ axleLift NOTIFY axleLiftChanged)
    // Hazards — DIN7 on the MCE18 expander (see decodeFrame()'s 0x702 case),
    // the one bit GAUGE-CLUSTER.md's MCE18 table left "reserved for a
    // possible future use". A static level like leftIndicator/rightIndicator
    // (on while engaged, off when cancelled), not the flasher's own
    // waveform — main.qml synthesizes the visible blink for all three off a
    // single shared clock, so its turn-signal icons can just OR this in with
    // leftIndicator/rightIndicator without the two falling out of phase.
    // Camera overlays are suppressed during hazards explicitly (main.qml's
    // leftCamOverlayActive/rightCamOverlayActive check !hazard) rather than
    // falling out for free, because on wiring where the hazard switch
    // flashes the same bulb circuits DIN0/DIN1 read, leftIndicator/
    // rightIndicator would read true during hazards too and otherwise pop
    // the cameras on their own.
    Q_PROPERTY(bool hazard READ hazard NOTIFY hazardChanged)
    // Cruise control — unlike the MCE18-sourced booleans above, this comes
    // from the Syvecs stream: cruiseState (Frame 2/0x601, slot 1), SCal enum
    // 0=OFF 1=ON 2=ACTIVE. The icon only distinguishes OFF/ACTIVE; ON (armed
    // but not actively controlling) reads as not-lit, same as OFF. The
    // dev-build simulator (see simulateTick()) still drives it (always true)
    // for layout testing.
    Q_PROPERTY(bool cruiseControl READ cruiseControl NOTIFY cruiseControlChanged)
    // Automatic/manual shift mode — not on the Syvecs fixed stream DBC, but
    // decoded from this car's CAN2 config as ManualAuto_U12 (Frame 6/0x605,
    // slot 3 — see decodeFrame()); polarity (nonzero = Automatic) is
    // assumed, not confirmed. Not sourced from the MCE18 expander. The
    // dev-build simulator (see simulateTick()) still drives it for layout
    // review.
    Q_PROPERTY(bool transmissionAuto READ transmissionAuto NOTIFY transmissionAutoChanged)
    // Drive mode selector — not on the Syvecs fixed stream. One of "SPORT",
    // "SPORT+", "RACE"; the dev-build simulator (see simulateTick()) cycles
    // through them for layout review.
    Q_PROPERTY(QString driveMode READ driveMode NOTIFY driveModeChanged)

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
    bool lowFuelWarn() const { return m_lowFuelWarn; }
    bool checkEngine() const { return m_checkEngine; }
    bool leftIndicator() const { return m_leftIndicator; }
    bool rightIndicator() const { return m_rightIndicator; }
    bool lowBeams() const { return m_lowBeams; }
    bool highBeams() const { return m_highBeams; }
    bool axleLift() const { return m_axleLift; }
    bool hazard() const { return m_hazard; }
    bool cruiseControl() const { return m_cruiseControl; }
    bool transmissionAuto() const { return m_transmissionAuto; }
    QString driveMode() const { return m_driveMode; }
    double oilPressure() const { return m_oilPressurePsi; }
    double vbat() const { return m_vbat; }
    QString cruiseState() const { return m_cruiseState; }
    int limpMode() const { return m_limpMode; }

    void setTotalOdo(double v);
    void setTripOdo(double v);

    // Debug-only keyboard trigger (see main.qml's 'L'/'R' Keys.onPressed) —
    // fully functional on every build, real hardware included. Confirmed on
    // real target hardware (2026-08-17) that a plugged-in USB keyboard's
    // key events reach Keys.onPressed just fine (kernel/evdev/Qt input all
    // work on this image). An earlier version gated the actual toggle logic
    // to simulate builds only, on the theory that a real board's turn
    // signals should only ever come from real CAN — but that's not a CAN
    // safety concern (this touches no bus, unlike the CAN1/CAN2 rules), and
    // it defeated the entire point of an on-board debug key: exercising the
    // overlay on real hardware without needing CAN connected. Ungated, this
    // simply overwrites m_leftIndicator/m_rightIndicator the same way a real
    // decodeFrame() DIN0/DIN1 update would — if real CAN is also connected
    // and driving these at the same time, whichever writes last wins, same
    // as any other debug override in this app (e.g. debugSetCameraGrid()).
    // debugToggleHazard() (main.qml's 'H' key) is the same pattern applied
    // to m_hazard/DIN7.
    Q_INVOKABLE void debugToggleLeftIndicator();
    Q_INVOKABLE void debugToggleRightIndicator();
    Q_INVOKABLE void debugToggleHazard();

    // Debug-only keyboard trigger (see main.qml's Up/Down Keys.onPressed) —
    // steps m_gear one position at a time through "PRN1234567", clamped at
    // both ends (0=P, 9=7th) rather than wrapping, since neither end has a
    // real gear beyond it to cycle into. On dev/simulate builds, the first
    // press latches m_simGearManualOverride so simulateTick()'s 2s
    // auto-cycle self-test (see its gear-cycle timer) stops overwriting
    // m_gear from then on — without that, the auto-cycle would silently undo
    // whatever the debug key just set within 2 seconds. On real hardware
    // there's nothing to latch against: m_gear only otherwise changes via
    // decodeFrame(), same as debugToggleLeftIndicator() et al.
    Q_INVOKABLE void debugGearUp();
    Q_INVOKABLE void debugGearDown();

    // Debug-only keyboard trigger (see main.qml's Space Keys.onPressed) —
    // steps through off -> low beams -> high beams -> off, one state at a
    // time, mutually exclusive (never both true at once) rather than high
    // beams adding onto low beams. On dev/simulate builds, the first press
    // latches m_simHeadlightsManualOverride so simulateTick()'s low-beam
    // auto-toggle self-test (see its "Headlights" comment) stops overwriting
    // m_lowBeams from then on — same reasoning as debugGearUp()/
    // debugGearDown() and m_simGearManualOverride. On real hardware there's
    // nothing to latch against: m_lowBeams/m_highBeams only otherwise change
    // via decodeFrame(), same as debugToggleLeftIndicator() et al.
    Q_INVOKABLE void debugCycleHeadlights();

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
    void lowFuelWarnChanged();
    void checkEngineChanged();
    void leftIndicatorChanged();
    void rightIndicatorChanged();
    void lowBeamsChanged();
    void highBeamsChanged();
    void axleLiftChanged();
    void hazardChanged();
    void cruiseControlChanged();
    void transmissionAutoChanged();
    void driveModeChanged();
    void oilPressureChanged();
    void vbatChanged();
    void cruiseStateChanged();
    void limpModeChanged();

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
    // Index into "PRN1234567": 0=P 1=R 2=N 3..9=1st..7th. Starts at P (0),
    // not N — before the first 0x60E frame ever arrives (app just launched,
    // CAN not yet connected), the car is realistically parked, so P is the
    // more honest guess to show than N. This is a different question from
    // decodeFrame()'s "Unknown"/unsupported-8th fallback once frames are
    // flowing, which stays Neutral on purpose — a live CAN glitch mid-drive
    // should read as the more neutral, less alarming N, not flash P.
    int m_gear = 0;
    double m_fuelLevel = 0.0;     // 0..1
    double m_coolantTempF = 0.0;
    double m_boostPsi = 0.0;
    double m_totalOdo = 0.0;
    double m_tripOdo = 0.0;
    bool m_oilPressureWarn = false;
    bool m_batteryWarn = false;
    bool m_coolantWarn = false;
    bool m_lowFuelWarn = false;   // fuelLevel < 1/4 tank
    bool m_checkEngine = false;
    bool m_leftIndicator = false;
    bool m_rightIndicator = false;
    bool m_lowBeams = false;
    bool m_highBeams = false;
    bool m_axleLift = false;
    bool m_hazard = false;
    bool m_cruiseControl = false;
    bool m_transmissionAuto = true;
    QString m_driveMode = QStringLiteral("SPORT");
    double m_oilPressurePsi = 0.0;             // raw reading behind m_oilPressureWarn
    double m_vbat = 0.0;                       // raw reading (V) behind m_batteryWarn
    QString m_cruiseState = QStringLiteral("OFF"); // raw OFF/ON/ACTIVE behind m_cruiseControl
    int m_limpMode = 0;                        // raw code behind m_checkEngine

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
    bool m_simGearManualOverride = false; // set by debugGearUp()/debugGearDown(); permanently stops the auto-cycle
    bool m_simHeadlightsManualOverride = false; // set by debugCycleHeadlights(); permanently stops the low-beam auto-toggle
    bool m_simOilFault = false;       // forces a low-oil-pressure dip to exercise the warn icon
    bool m_simBattFault = false;      // forces a low-voltage dip to exercise the warn icon
#endif
};

#endif
