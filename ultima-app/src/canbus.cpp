#include "canbus.h"
#include "odostore.h"

#include <QDateTime>
#include <QtGlobal>
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
#include <QRandomGenerator>
#include <cmath>
#endif

#include <stdio.h>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#endif

// Syvecs S7+ CAN frames are 8 bytes carrying four 16-bit big-endian quantities
// in slots 1..4 at byte offsets 0-1, 2-3, 4-5, 6-7. This car's CAN2 layout is
// a custom Generic CAN Transmit config (NOT a mirror of the CAN1 fixed stream
// DBC); verified frame map below was read from SCal Datastreams → Generic CAN
// Transmit → Transmit Content. Channel scalings/signedness are the Syvecs
// per-channel defaults documented in the published S7 Fixed Stream v3 DBC.

static inline quint16 be_u16(const quint8 *d, int off) {
    return (quint16(d[off]) << 8) | quint16(d[off + 1]);
}
static inline qint16 be_s16(const quint8 *d, int off) {
    return qint16(be_u16(d, off));
}

// MCE18 CAN bus expander (CANchecked-protocol) — supplies the analog/digital
// inputs the Syvecs S7+ has no channel for at all (fuel sender, turn
// signals, beams, cruise, axle lift, auto/manual). See "CAN2 MCE18 Mapping"
// PDF and GAUGE-CLUSTER.md's MCE18 section for the full frame layout and the
// assumptions below — none of this is wire-verified yet (no unit on the
// bench), unlike the Syvecs frame map above.
//
// TX Base ID is user-configurable on the unit; 0x700 is its datasheet
// default and doesn't collide with the Syvecs frames (0x600-0x614), but
// isn't confirmed as this car's actual configured value.
static constexpr quint32 kMce18Base = 0x700;
// AIN0-8 can be configured on the unit as either raw 0-1023 ADC counts or
// pre-scaled 0-5000mV — the datasheet doesn't say which is the power-on
// default, and it isn't confirmed for this unit. Assuming raw counts, with
// the ADC's full-scale (1023 counts) representing 5000mV.
static constexpr double kMce18AinRawMax = 1023.0;
static constexpr double kMce18AinFullScaleMv = 5000.0;
// Fuel sender: 0V empty, 4V full (per sender spec) — narrower than the AIN's
// own 0-5V full scale, so this is a second, independent conversion on top
// of the raw-counts-to-mV one above.
static constexpr double kFuelSenderFullScaleMv = 4000.0;

CanBus::CanBus(OdoStore *odo, const QString &iface, QObject *parent)
    : QObject(parent), m_odo(odo), m_iface(iface)
{
    if (m_odo) {
        m_totalOdo = m_odo->totalOdo();
        m_tripOdo = m_odo->tripOdo();
    }
    initDiag();
    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CanBus::tryConnect);
    tryConnect();
}

CanBus::~CanBus()
{
    closeSocket();
}

// Seeds every diagnostics-screen key with a resting default so QML bindings
// never see an undefined value — on real hardware this is the only thing
// that ever populates m_diag, since decodeFrame() doesn't decode any of
// these channels yet (see the Q_PROPERTY comment in canbus.h).
void CanBus::initDiag()
{
    m_diag = {
        // Air / fuel delivery
        { QStringLiteral("pedalPct"), 0.0 }, { QStringLiteral("throttlePct"), 0.0 },
        { QStringLiteral("dbwTargetPct"), 0.0 }, { QStringLiteral("chargeAirTempC"), 25.0 },
        { QStringLiteral("wastegateTargetKpa"), 100.0 }, { QStringLiteral("loadPct"), 0.0 },
        { QStringLiteral("throttleClosed"), true }, { QStringLiteral("runMode"), QStringLiteral("IDLE") },
        // Ignition & knock
        { QStringLiteral("ignTimingDeg"), 12.0 },
        { QStringLiteral("knock1"), 0.0 }, { QStringLiteral("knock2"), 0.0 }, { QStringLiteral("knock3"), 0.0 },
        { QStringLiteral("knock4"), 0.0 }, { QStringLiteral("knock5"), 0.0 }, { QStringLiteral("knock6"), 0.0 },
        { QStringLiteral("fuelTrimCell"), 1.0 },
        // Fuel system
        { QStringLiteral("lambda1"), 1.0 }, { QStringLiteral("lambda2"), 1.0 },
        { QStringLiteral("railPressureKpa"), 300.0 }, { QStringLiteral("injectorDutyPct"), 0.0 },
        { QStringLiteral("fuelConsRateCcMin"), 0.0 }, { QStringLiteral("fuelCompPct"), 0.0 },
        { QStringLiteral("pump1"), false }, { QStringLiteral("pump2"), false }, { QStringLiteral("pump3"), false },
        { QStringLiteral("vbatCompMs"), 0.3 },
        // Drivetrain & torque
        { QStringLiteral("wheelSpinPct"), 0.0 }, { QStringLiteral("tcSpinErrPct"), 0.0 },
        { QStringLiteral("tcTorqueCutPct"), 0.0 }, { QStringLiteral("launchRpm"), 4500.0 },
        { QStringLiteral("torqueOutputNm"), 0.0 }, { QStringLiteral("torqueDemandNm"), 0.0 },
        { QStringLiteral("calSelect"), 2 }, { QStringLiteral("pitLimiter"), false },
        // Trans / unconfirmed (raw TCM channels, meaning not verified)
        { QStringLiteral("manualAuto"), true }, { QStringLiteral("paddleDown"), false },
        { QStringLiteral("sportPlus"), false }, { QStringLiteral("clutchAPressureKpa"), 300.0 },
        { QStringLiteral("clutchBPressureKpa"), 300.0 }, { QStringLiteral("tcmLimp"), false },
        { QStringLiteral("tcmLogging"), 1 }, { QStringLiteral("carDtc"), 0 },
    };
}

void CanBus::closeSocket()
{
#ifdef __linux__
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}

void CanBus::tryConnect()
{
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
    if (m_fd >= 0)
        return;

    int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "[canbus] socket(PF_CAN): %s\n", strerror(errno));
        m_reconnectTimer.start();
        return;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, m_iface.toLocal8Bit().constData(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        // Interface not present yet — udev hasn't brought it up.
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // Most common reason here: link is administratively down.
        fprintf(stderr, "[canbus] bind(%s): %s\n",
                qPrintable(m_iface), strerror(errno));
        ::close(fd);
        m_reconnectTimer.start();
        return;
    }

    m_fd = fd;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &CanBus::onReadable);
    m_reconnectTimer.stop();
    fprintf(stderr, "[canbus] connected to %s\n", qPrintable(m_iface));
#else
    // SocketCAN is Linux-only, and even on Linux this branch only compiles
    // for dev builds (CONFIG+=ultima_dev_sim). Drive the gauges with a
    // simulated data stream instead (see simulateTick()) so the QML can be
    // exercised without real CAN hardware. tryConnect() only runs once here
    // (nothing re-triggers it), so this wiring happens exactly once.
    fprintf(stderr, "[canbus] SocketCAN unavailable/disabled for this build — simulating data\n");
    connect(&m_simTimer, &QTimer::timeout, this, &CanBus::simulateTick);
    m_simTimer.start(60);
#endif
}

void CanBus::onReadable()
{
#ifdef __linux__
    struct can_frame frame;
    for (int i = 0; i < 64; ++i) {     // drain up to 64 per wake
        ssize_t n = ::read(m_fd, &frame, sizeof(frame));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            // Bus error / interface went down — close and retry.
            fprintf(stderr, "[canbus] read: %s — reconnecting\n", strerror(errno));
            closeSocket();
            m_reconnectTimer.start();
            return;
        }
        if (n != (ssize_t)sizeof(frame))
            return;
        // Skip error/RTR/extended frames — Syvecs uses 11-bit data frames.
        if (frame.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG | CAN_EFF_FLAG))
            continue;
        decodeFrame(frame.can_id & CAN_SFF_MASK, frame.data, frame.can_dlc);
    }
#endif
}

void CanBus::accumulateOdometer()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastSpeedMs != 0 && m_speed > 0) {
        double dt = (now - m_lastSpeedMs) / 1000.0;
        if (dt > 0 && dt < 1.0) {           // ignore stalls
            double miles = m_speed * dt / 3600.0;
            m_totalOdo += miles;
            m_tripOdo += miles;
            emit totalOdoChanged();
            emit tripOdoChanged();
        }
    }
    m_lastSpeedMs = now;
}

void CanBus::decodeFrame(quint32 id, const quint8 *d, int dlc)
{
    if (dlc < 8)
        return;

    switch (id) {
    case 0x600: {                                       // Frame 1: rpm @ slot 1, map1A @ slot 4
        double v = qMax(0, int(be_s16(d, 0)));
        if (v != m_rpm) { m_rpm = v; emit rpmChanged(); }

        // map1A (SCal Slot 4/Frame 1): y=(1*x)+0, signed, absolute MAP in
        // mbar. Signed matters — near vacuum a raw negative read through an
        // unsigned decode would wrap to ~65500 and peg the gauge. Subtract
        // standard atmosphere (1013.25 mbar) to get boost above ambient.
        double mapMbar = be_s16(d, 6);
        double boostPsi = qMax(0.0, (mapMbar - 1013.25) * 0.0145038);
        if (!qFuzzyCompare(1.0 + boostPsi, 1.0 + m_boostPsi)) {
            m_boostPsi = boostPsi;
            emit boostChanged();
        }
        break;
    }
    case 0x604: {                                       // Frame 5: limpMode @ slot 4
        int limp = be_u16(d, 6);
        // sensorWarningLevel is not on CAN2 in this config — derive
        // checkEngine from limpMode alone.
        bool ce = (limp != 0);
        if (ce != m_checkEngine) { m_checkEngine = ce; emit checkEngineChanged(); }
        break;
    }
    case 0x605: {                                       // Frame 6: ect1 @ slot 2, ManualAuto_U12 @ slot 3
        double c = be_s16(d, 2) * 0.1;
        double f = c * 1.8 + 32.0;
        if (!qFuzzyCompare(1.0 + f, 1.0 + m_coolantTempF)) {
            m_coolantTempF = f;
            emit coolantTempChanged();
        }
        bool warn = f > 220.0;
        if (warn != m_coolantWarn) { m_coolantWarn = warn; emit coolantWarnChanged(); }

        // ManualAuto_U12: raw TCM enum: polarity assumed (nonzero =
        // Automatic) — not confirmed against a SCal screenshot or candump.
        bool transAuto = be_u16(d, 4) != 0;
        if (transAuto != m_transmissionAuto) { m_transmissionAuto = transAuto; emit transmissionAutoChanged(); }
        break;
    }
    case 0x608: {                                       // Frame 9: eop1 @ slot 1
        double eopKpa = be_s16(d, 0) * 0.1;
        double eopPsi = eopKpa * 0.145038;
        bool warn = (m_rpm >= 600.0) && (eopPsi <= 40.0);
        if (warn != m_oilPressureWarn) {
            m_oilPressureWarn = warn;
            emit oilPressureWarnChanged();
        }
        break;
    }
    case 0x60E: {                                       // Frame 15: gear @ slot 2, vbat @ slot 3
        int g = be_s16(d, 2);
        // Syvecs: 0=Unknown 1=Reverse 2=Neutral 3=1st .. 10=8th
        // QML:    -2=P -1=R 0=N 1..8=forward
        int qmlGear;
        if (g == 1)                  qmlGear = -1;
        else if (g == 2)             qmlGear = 0;
        else if (g >= 3 && g <= 10)  qmlGear = g - 2;
        else                         qmlGear = 0;       // Unknown → Neutral
        if (qmlGear != m_gear) { m_gear = qmlGear; emit gearChanged(); }

        double vbat = be_u16(d, 4) * 0.001;
        bool warn = vbat < 12.5;
        if (warn != m_batteryWarn) { m_batteryWarn = warn; emit batteryWarnChanged(); }
        break;
    }
    case 0x60F: {                                       // Frame 16: vehicleSpeed @ slot 1
        // Slots 2/3/4 also carry vehicleSpeed (driven/gps/etc.); slot 1 is the
        // primary reading and what drives the gauge + odometer.
        accumulateOdometer();
        double kph = be_s16(d, 0) * 0.036;
        double mph = qMax(0.0, kph * 0.621371);
        if (!qFuzzyCompare(1.0 + mph, 1.0 + m_speed)) {
            m_speed = mph;
            emit speedChanged();
        }
        break;
    }
    // flvlA isn't broadcast on the ECU's CAN2 config — fuel level instead
    // comes from the MCE18 expander below (AIN0), not the Syvecs frames.
    case kMce18Base: {                                   // MCE18 frame 1: AIN0-3
        // Only AIN0 (fuel sender) is wired up; AIN1-3 unused for now.
        // Fuel sender is 0V empty / 4V full (linear, per sender spec) —
        // convert raw counts to mV against the AIN's own 0-5V full scale
        // first, then scale that against the sender's narrower 0-4V range.
        double mv = be_u16(d, 0) * (kMce18AinFullScaleMv / kMce18AinRawMax);
        double fuel = qBound(0.0, mv / kFuelSenderFullScaleMv, 1.0);
        if (!qFuzzyCompare(1.0 + fuel, 1.0 + m_fuelLevel)) {
            m_fuelLevel = fuel;
            emit fuelLevelChanged();
        }
        break;
    }
    case kMce18Base + 2: {                               // MCE18 frame @ Base ID+2: AIN8, DIN0-7 mask @ byte 2
        // Bit N = DIN N (assumed — datasheet doesn't spell out bit order).
        // DIN6 is left unassigned: the datasheet's Frequency-1 input reuses
        // that same pin (**TX Base ID+3, "Frequency 1 - DIN6"), so it's kept
        // free rather than double-booked. DIN7 is also unassigned: Auto/Manual
        // comes from the Syvecs stream instead (see the 0x605 case above),
        // not this expander.
        quint8 dinMask = d[2];
        bool leftInd = dinMask & (1 << 0);
        bool rightInd = dinMask & (1 << 1);
        bool axleLiftIn = dinMask & (1 << 2);
        bool lowBeamsIn = dinMask & (1 << 3);
        bool highBeamsIn = dinMask & (1 << 4);
        bool cruiseIn = dinMask & (1 << 5);

        if (leftInd != m_leftIndicator) { m_leftIndicator = leftInd; emit leftIndicatorChanged(); }
        if (rightInd != m_rightIndicator) { m_rightIndicator = rightInd; emit rightIndicatorChanged(); }
        if (axleLiftIn != m_axleLift) { m_axleLift = axleLiftIn; emit axleLiftChanged(); }
        if (lowBeamsIn != m_lowBeams) { m_lowBeams = lowBeamsIn; emit lowBeamsChanged(); }
        if (highBeamsIn != m_highBeams) { m_highBeams = highBeamsIn; emit highBeamsChanged(); }
        if (cruiseIn != m_cruiseControl) { m_cruiseControl = cruiseIn; emit cruiseControlChanged(); }
        break;
    }
    default:
        break;
    }
}

void CanBus::setTotalOdo(double v)
{
    if (qFuzzyCompare(1.0 + v, 1.0 + m_totalOdo))
        return;
    m_totalOdo = v;
    emit totalOdoChanged();
}

void CanBus::setTripOdo(double v)
{
    if (qFuzzyCompare(1.0 + v, 1.0 + m_tripOdo))
        return;
    m_tripOdo = v;
    emit tripOdoChanged();
}

void CanBus::save()
{
    if (!m_odo)
        return;
    m_odo->setTotalOdo(m_totalOdo);
    m_odo->setTripOdo(m_tripOdo);
    m_odo->save();
}

#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
// Dev-build data simulator. Ports SimEngine.qml's phase-based driving
// profile (random city/stop/suburban/highway/spirited legs) onto CanBus's
// mph/°F/psi units so main.qml's gauges animate without real CAN hardware.
void CanBus::simulateTick()
{
    const double dt = 0.06; // matches m_simTimer interval (60 ms)
    const double kTwoPi = 6.283185307179586;
    auto rnd = []() { return QRandomGenerator::global()->generateDouble(); };

    m_simElapsedS += dt;

    // Headlights: toggle low beams on/off every 2s so the car art and
    // low-beam icon can be exercised without real CAN hardware.
    bool lowBeams = std::fmod(m_simElapsedS, 4.0) < 2.0;
    if (lowBeams != m_lowBeams) { m_lowBeams = lowBeams; emit lowBeamsChanged(); }

    // Cruise control: always engaged in the simulator so the icon can be
    // exercised without real CAN hardware.
    if (!m_cruiseControl) { m_cruiseControl = true; emit cruiseControlChanged(); }

    // Transmission mode: flip Automatic/Manual every few seconds so the
    // gear indicator's A/M badge can be exercised without real CAN hardware.
    bool transmissionAuto = std::fmod(m_simElapsedS, 6.0) < 3.0;
    if (transmissionAuto != m_transmissionAuto) {
        m_transmissionAuto = transmissionAuto;
        emit transmissionAutoChanged();
    }

    // Drive mode: step through SPORT -> SPORT+ -> RACE on a slow cycle so
    // the indicator's three colors can be exercised without real CAN
    // hardware.
    static const QString driveModes[] = { QStringLiteral("SPORT"), QStringLiteral("SPORT+"), QStringLiteral("RACE") };
    int driveModeIndex = int(std::fmod(m_simElapsedS / 5.0, 3.0));
    if (driveModes[driveModeIndex] != m_driveMode) {
        m_driveMode = driveModes[driveModeIndex];
        emit driveModeChanged();
    }

    // Fuel/coolant: sweep the full gauge range on independent slow sine
    // waves (real hardware doesn't broadcast fuel level at all, and
    // coolant only wanders a few degrees in practice — this is purely so
    // the gauges can be seen sweeping end-to-end during layout review).
    double fuelLevel = 0.5 + 0.5 * std::sin(kTwoPi * m_simElapsedS / 24.0);
    if (!qFuzzyCompare(1.0 + fuelLevel, 1.0 + m_fuelLevel)) {
        m_fuelLevel = fuelLevel;
        emit fuelLevelChanged();
    }

    m_simPhaseTimer -= dt;
    if (m_simPhaseTimer <= 0.0) {
        struct Phase { double target, dur; };
        const Phase phases[] = {
            { 20.0 + rnd() * 15.0,  8.0 + rnd() * 6.0  },  // city
            { 0.0,                  3.0 + rnd() * 3.0  },  // stop
            { 35.0 + rnd() * 20.0, 10.0 + rnd() * 8.0  },  // suburban
            { 55.0 + rnd() * 30.0, 12.0 + rnd() * 10.0 }, // highway
            { 75.0 + rnd() * 45.0,  8.0 + rnd() * 6.0  },  // spirited
        };
        const Phase &phase = phases[int(rnd() * 5)];
        m_simTargetSpeed = phase.target;
        m_simPhaseTimer = phase.dur;

        if (rnd() < 0.3) { m_oilPressureWarn = !m_oilPressureWarn; emit oilPressureWarnChanged(); }
        if (rnd() < 0.4) { m_checkEngine = !m_checkEngine; emit checkEngineChanged(); }
        if (rnd() < 0.3) { m_batteryWarn = !m_batteryWarn; emit batteryWarnChanged(); }
    }

    accumulateOdometer(); // uses m_speed/timestamp from before this tick's update

    m_simAccel = (m_simTargetSpeed - m_speed) * 0.02;
    double noise = (rnd() - 0.5) * 0.6;
    double newSpeed = qBound(0.0, m_speed + m_simAccel + noise, 160.0);
    if (!qFuzzyCompare(1.0 + newSpeed, 1.0 + m_speed)) { m_speed = newSpeed; emit speedChanged(); }

    // Gear: cycle R N P 1..7 on a fixed timer (independent of simulated
    // speed) so the gear indicator and transmission-mode badge can be
    // exercised for every glyph without real CAN hardware.
    m_simGearCycleTimer -= dt;
    if (m_simGearCycleTimer <= 0.0) {
        static const int gearSequence[] = { -1, 0, -2, 1, 2, 3, 4, 5, 6, 7 };
        const int gearSequenceLen = int(sizeof(gearSequence) / sizeof(gearSequence[0]));
        int newGear = gearSequence[m_simGearCycleIndex];
        if (newGear != m_gear) { m_gear = newGear; emit gearChanged(); }
        m_simGearCycleIndex = (m_simGearCycleIndex + 1) % gearSequenceLen;
        m_simGearCycleTimer = 2.0;
    }

    double newRpm;
    if (m_gear <= 0) {
        newRpm = 800.0 + rnd() * 100.0;
    } else {
        static const double ratios[] = { 0, 3.5, 2.5, 1.8, 1.4, 1.1, 0.9, 0.75, 0.65 };
        double base = m_speed * ratios[m_gear] * 30.0;
        newRpm = qBound(800.0, base + (rnd() - 0.5) * 200.0, 7200.0);
    }
    if (!qFuzzyCompare(1.0 + newRpm, 1.0 + m_rpm)) { m_rpm = newRpm; emit rpmChanged(); }

    double targetCoolant = 200.0 + 40.0 * std::sin(kTwoPi * m_simElapsedS / 30.0) + (rnd() - 0.5) * 2.0;
    double newCoolant = m_coolantTempF + (targetCoolant - m_coolantTempF) * 0.01;
    if (!qFuzzyCompare(1.0 + newCoolant, 1.0 + m_coolantTempF)) {
        m_coolantTempF = newCoolant;
        emit coolantTempChanged();
    }
    bool coolantWarn = m_coolantTempF > 220.0;
    if (coolantWarn != m_coolantWarn) { m_coolantWarn = coolantWarn; emit coolantWarnChanged(); }

    double boostTarget = 0.0;
    if (m_speed > 20.0 && m_simAccel > 0.0)
        boostTarget = qMin(24.0, m_simAccel * 200.0 + m_rpm / 300.0);
    double newBoost = m_boostPsi + (boostTarget - m_boostPsi) * 0.05;
    if (!qFuzzyCompare(1.0 + newBoost, 1.0 + m_boostPsi)) { m_boostPsi = newBoost; emit boostChanged(); }

    // ---- Diagnostics-screen channels (see canbus.h Q_PROPERTY comment) ----
    // Not physically modeled — just plausible-looking values correlated with
    // the driving state already computed above, for diagnostic-screen layout
    // review without real CAN hardware.
    double pedalPct = qBound(0.0, m_simAccel * 70.0 + m_speed * 0.15 + (rnd() - 0.5) * 3.0, 100.0);
    double throttlePct = qBound(0.0, pedalPct + (rnd() - 0.5) * 3.0, 100.0);
    double dbwTargetPct = qBound(0.0, throttlePct + (rnd() - 0.5) * 2.0, 100.0);
    double loadPct = qBound(0.0, (m_rpm / 7200.0) * 60.0 + throttlePct * 0.35, 100.0);
    double chargeAirTempC = 25.0 + m_boostPsi * 0.6 + (rnd() - 0.5) * 1.0;
    double wastegateTargetKpa = 100.0 + m_boostPsi * 3.0;
    bool throttleClosed = throttlePct < 2.0;
    QString runMode = (m_speed < 1.0 && m_rpm < 1000.0) ? QStringLiteral("IDLE") : QStringLiteral("RUN");

    // Knock: silent almost all the time, with an occasional randomized
    // per-cylinder blip that decays back to zero — this is the whole reason
    // the diagnostic screen exists (spot the one cylinder misbehaving), so
    // the simulator should actually produce that case sometimes.
    m_simKnockEventTimer -= dt;
    if (m_simKnockEventTimer <= 0.0) {
        m_simKnock[int(rnd() * 6.0)] = 1.0 + rnd() * 2.0;
        m_simKnockEventTimer = 6.0 + rnd() * 10.0;
    }
    double knockSum = 0.0;
    for (int i = 0; i < 6; ++i) { m_simKnock[i] *= 0.93; knockSum += m_simKnock[i]; }
    double ignTimingDeg = 12.0 + (m_rpm / 7200.0) * 20.0 - knockSum * 0.5 + (rnd() - 0.5) * 0.5;
    double fuelTrimCell = 1.0 + (rnd() - 0.5) * 0.06;

    double lambda1 = 1.0 + std::sin(kTwoPi * m_simElapsedS / 5.0) * 0.03 + (rnd() - 0.5) * 0.01;
    double lambda2 = 1.0 + std::sin(kTwoPi * m_simElapsedS / 5.3 + 1.0) * 0.03 + (rnd() - 0.5) * 0.01;
    double railPressureKpa = 300.0 + m_rpm * 0.02 + (rnd() - 0.5) * 5.0;
    double injectorDutyPct = qBound(0.0, loadPct * 0.7 + (rnd() - 0.5) * 2.0, 100.0);
    double fuelConsRateCcMin = qMax(0.0, 15.0 + m_rpm * 0.02 + throttlePct * 0.5);
    double fuelCompPct = (rnd() - 0.5) * 4.0;
    bool pump1 = m_rpm > 400.0;
    bool pump2 = loadPct > 50.0;
    bool pump3 = m_rpm > 5500.0;
    double vbatCompMs = 0.3 + (rnd() - 0.5) * 0.1;

    double wheelSpinPct = qMax(0.0, m_simAccel * 2.5 + (rnd() - 0.5) * 0.6);
    double tcSpinErrPct = wheelSpinPct - 3.0 + (rnd() - 0.5) * 0.5;
    double tcTorqueCutPct = qMax(0.0, (wheelSpinPct - 3.0) * 2.0);
    double torqueOutputNm = 30.0 + (m_rpm / 7200.0) * 350.0 * (0.4 + throttlePct / 100.0 * 0.6);
    double torqueDemandNm = torqueOutputNm + (pedalPct - throttlePct) * 2.0;
    bool manualAuto = m_transmissionAuto;
    bool paddleDown = m_simGearCycleTimer > 1.95; // brief pulse right after each simulated shift
    bool sportPlus = (m_driveMode == QStringLiteral("SPORT+"));
    double clutchAPressureKpa = 300.0 + torqueOutputNm * 0.3 + (rnd() - 0.5) * 5.0;
    double clutchBPressureKpa = 300.0 + torqueOutputNm * 0.3 + (rnd() - 0.5) * 5.0;
    // carDtc: left at initDiag()'s static default. It's tempting to derive
    // this from m_checkEngine, but that flag toggles randomly for the top
    // indicator row's own demo purposes and has nothing to do with the
    // (unconfirmed, never-decoded) TCM DTC channel this would represent —
    // coupling them would make the Trans page flicker a fault count that
    // means nothing.

    m_diag[QStringLiteral("pedalPct")] = pedalPct;
    m_diag[QStringLiteral("throttlePct")] = throttlePct;
    m_diag[QStringLiteral("dbwTargetPct")] = dbwTargetPct;
    m_diag[QStringLiteral("chargeAirTempC")] = chargeAirTempC;
    m_diag[QStringLiteral("wastegateTargetKpa")] = wastegateTargetKpa;
    m_diag[QStringLiteral("loadPct")] = loadPct;
    m_diag[QStringLiteral("throttleClosed")] = throttleClosed;
    m_diag[QStringLiteral("runMode")] = runMode;
    m_diag[QStringLiteral("ignTimingDeg")] = ignTimingDeg;
    m_diag[QStringLiteral("knock1")] = m_simKnock[0];
    m_diag[QStringLiteral("knock2")] = m_simKnock[1];
    m_diag[QStringLiteral("knock3")] = m_simKnock[2];
    m_diag[QStringLiteral("knock4")] = m_simKnock[3];
    m_diag[QStringLiteral("knock5")] = m_simKnock[4];
    m_diag[QStringLiteral("knock6")] = m_simKnock[5];
    m_diag[QStringLiteral("fuelTrimCell")] = fuelTrimCell;
    m_diag[QStringLiteral("lambda1")] = lambda1;
    m_diag[QStringLiteral("lambda2")] = lambda2;
    m_diag[QStringLiteral("railPressureKpa")] = railPressureKpa;
    m_diag[QStringLiteral("injectorDutyPct")] = injectorDutyPct;
    m_diag[QStringLiteral("fuelConsRateCcMin")] = fuelConsRateCcMin;
    m_diag[QStringLiteral("fuelCompPct")] = fuelCompPct;
    m_diag[QStringLiteral("pump1")] = pump1;
    m_diag[QStringLiteral("pump2")] = pump2;
    m_diag[QStringLiteral("pump3")] = pump3;
    m_diag[QStringLiteral("vbatCompMs")] = vbatCompMs;
    m_diag[QStringLiteral("wheelSpinPct")] = wheelSpinPct;
    m_diag[QStringLiteral("tcSpinErrPct")] = tcSpinErrPct;
    m_diag[QStringLiteral("tcTorqueCutPct")] = tcTorqueCutPct;
    // launchRpm, calSelect, pitLimiter, tcmLimp, tcmLogging: static config-like
    // values on real hardware — left at initDiag()'s defaults, nothing to simulate.
    m_diag[QStringLiteral("torqueOutputNm")] = torqueOutputNm;
    m_diag[QStringLiteral("torqueDemandNm")] = torqueDemandNm;
    m_diag[QStringLiteral("manualAuto")] = manualAuto;
    m_diag[QStringLiteral("paddleDown")] = paddleDown;
    m_diag[QStringLiteral("sportPlus")] = sportPlus;
    m_diag[QStringLiteral("clutchAPressureKpa")] = clutchAPressureKpa;
    m_diag[QStringLiteral("clutchBPressureKpa")] = clutchBPressureKpa;
    // carDtc intentionally not updated here — see comment above.
    emit diagChanged();

    if (!m_diagLive) { m_diagLive = true; emit diagLiveChanged(); }
}
#endif
