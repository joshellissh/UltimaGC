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
// signals, beams, axle lift). See "CAN2 MCE18 Mapping"
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
// Fuel sender: 1V empty, 4V full (per sender spec) — narrower than the AIN's
// own 0-5V full scale, so this is a second, independent conversion on top
// of the raw-counts-to-mV one above.
static constexpr double kFuelSenderEmptyMv = 1000.0;
static constexpr double kFuelSenderFullScaleMv = 4000.0;

CanBus::CanBus(OdoStore *odo, const QString &iface, QObject *parent)
    : QObject(parent), m_odo(odo), m_iface(iface)
{
    if (m_odo) {
        m_totalOdo = m_odo->totalOdo();
        m_tripOdo = m_odo->tripOdo();
    }
    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CanBus::tryConnect);
    tryConnect();
}

CanBus::~CanBus()
{
    closeSocket();
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
    case 0x601: {                                       // Frame 2: cruiseState @ slot 1
        // cruiseState: SCal enum 0=OFF 1=ON 2=ACTIVE. Icon only
        // distinguishes OFF/ACTIVE — ON (armed but not actively
        // controlling) reads as not-lit, same as OFF.
        int cruiseStateRaw = be_u16(d, 0);
        static const QString kCruiseStateNames[] = { QStringLiteral("OFF"), QStringLiteral("ON"), QStringLiteral("ACTIVE") };
        QString cruiseState = (cruiseStateRaw <= 2) ? kCruiseStateNames[cruiseStateRaw] : QStringLiteral("?");
        if (cruiseState != m_cruiseState) { m_cruiseState = cruiseState; emit cruiseStateChanged(); }
        bool cruiseActive = (cruiseStateRaw == 2);
        if (cruiseActive != m_cruiseControl) { m_cruiseControl = cruiseActive; emit cruiseControlChanged(); }
        break;
    }
    case 0x604: {                                       // Frame 5: limpMode @ slot 4
        int limp = be_u16(d, 6);
        if (limp != m_limpMode) { m_limpMode = limp; emit limpModeChanged(); }
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
        if (!qFuzzyCompare(1.0 + eopPsi, 1.0 + m_oilPressurePsi)) {
            m_oilPressurePsi = eopPsi;
            emit oilPressureChanged();
        }
        bool warn = (m_rpm >= 600.0) && (eopPsi <= 40.0);
        if (warn != m_oilPressureWarn) {
            m_oilPressureWarn = warn;
            emit oilPressureWarnChanged();
        }
        break;
    }
    case 0x60E: {                                       // Frame 15: gear @ slot 2, vbat @ slot 3
        int g = be_s16(d, 2);
        // Syvecs: 0=Unknown 1=Reverse 2=Neutral 3=1st .. 10=8th (this car
        // only has 7 forward gears — see main.qml's "PRN1234567" comment).
        // QML:    0=P 1=R 2=N 3..9=1st..7th — Syvecs' 3=1st..9=7th already
        // equals the QML index directly, no offset needed. Syvecs has no
        // Park value at all (TCM range, not something this standalone ECU
        // tracks) so qmlGear 0 is unreachable from real CAN, same as before
        // this encoding change — only debugToggle-free, simulateTick()
        // (dev builds) can produce it.
        int qmlGear;
        if (g == 1)                  qmlGear = 1;        // Reverse
        else if (g >= 3 && g <= 9)   qmlGear = g;         // 1st..7th
        else                         qmlGear = 2;         // Neutral, Unknown, or unsupported 8th (10)
        if (qmlGear != m_gear) { m_gear = qmlGear; emit gearChanged(); }

        double vbat = be_u16(d, 4) * 0.001;
        if (!qFuzzyCompare(1.0 + vbat, 1.0 + m_vbat)) { m_vbat = vbat; emit vbatChanged(); }
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
        // Fuel sender is 1V empty / 4V full (linear, per sender spec) —
        // convert raw counts to mV against the AIN's own 0-5V full scale
        // first, then scale that against the sender's narrower 1-4V range.
        double mv = be_u16(d, 0) * (kMce18AinFullScaleMv / kMce18AinRawMax);
        double fuel = qBound(0.0, (mv - kFuelSenderEmptyMv) / (kFuelSenderFullScaleMv - kFuelSenderEmptyMv), 1.0);
        if (!qFuzzyCompare(1.0 + fuel, 1.0 + m_fuelLevel)) {
            m_fuelLevel = fuel;
            emit fuelLevelChanged();
        }
        break;
    }
    case kMce18Base + 2: {                               // MCE18 frame @ Base ID+2: AIN8, DIN0-7 mask @ byte 2
        // Bit N = DIN N (assumed — datasheet doesn't spell out bit order).
        // DIN5 is left unassigned: cruise comes from the Syvecs stream
        // instead (see the 0x601 case above), not this expander. DIN6 is
        // also unassigned: the datasheet's Frequency-1 input reuses that
        // same pin (**TX Base ID+3, "Frequency 1 - DIN6"), so it's kept free
        // rather than double-booked. DIN7 is hazard — same treatment as
        // DIN0/DIN1 (leftInd/rightInd): confirmed a static "switch pressed"
        // level (on while engaged, off the instant it's cancelled), not the
        // flasher's own on/off waveform — so this is a plain pass-through
        // with no debounce/synthesis. The dash's visible blink is
        // synthesized from this level in QML instead (see main.qml's
        // blinkTimer) rather than read off the CAN signal directly.
        quint8 dinMask = d[2];
        bool leftInd = dinMask & (1 << 0);
        bool rightInd = dinMask & (1 << 1);
        bool axleLiftIn = dinMask & (1 << 2);
        bool lowBeamsIn = dinMask & (1 << 3);
        bool highBeamsIn = dinMask & (1 << 4);
        bool hazardIn = dinMask & (1 << 7);

        if (leftInd != m_leftIndicator) { m_leftIndicator = leftInd; emit leftIndicatorChanged(); }
        if (rightInd != m_rightIndicator) { m_rightIndicator = rightInd; emit rightIndicatorChanged(); }
        if (axleLiftIn != m_axleLift) { m_axleLift = axleLiftIn; emit axleLiftChanged(); }
        if (lowBeamsIn != m_lowBeams) { m_lowBeams = lowBeamsIn; emit lowBeamsChanged(); }
        if (highBeamsIn != m_highBeams) { m_highBeams = highBeamsIn; emit highBeamsChanged(); }
        if (hazardIn != m_hazard) { m_hazard = hazardIn; emit hazardChanged(); }
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
    if (m_cruiseState != QStringLiteral("ACTIVE")) { m_cruiseState = QStringLiteral("ACTIVE"); emit cruiseStateChanged(); }

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

        if (rnd() < 0.3) m_simOilFault = !m_simOilFault;
        if (rnd() < 0.4) {
            m_checkEngine = !m_checkEngine;
            emit checkEngineChanged();
            int limp = m_checkEngine ? 1 : 0;
            if (limp != m_limpMode) { m_limpMode = limp; emit limpModeChanged(); }
        }
        if (rnd() < 0.3) m_simBattFault = !m_simBattFault;
    }

    accumulateOdometer(); // uses m_speed/timestamp from before this tick's update

    m_simAccel = (m_simTargetSpeed - m_speed) * 0.02;
    double noise = (rnd() - 0.5) * 0.6;
    double newSpeed = qBound(0.0, m_speed + m_simAccel + noise, 160.0);
    if (!qFuzzyCompare(1.0 + newSpeed, 1.0 + m_speed)) { m_speed = newSpeed; emit speedChanged(); }

    // Gear: cycle R N P 1..7 on a fixed timer (independent of simulated
    // speed) so the gear indicator and transmission-mode badge can be
    // exercised for every glyph without real CAN hardware. Values are
    // indices into "PRN1234567" (see canbus.h's gear Q_PROPERTY comment):
    // 0=P 1=R 2=N 3..9=1st..7th. Skipped for good once debugGearUp()/
    // debugGearDown() sets m_simGearManualOverride — see canbus.h's comment
    // on those.
    m_simGearCycleTimer -= dt;
    if (!m_simGearManualOverride && m_simGearCycleTimer <= 0.0) {
        static const int gearSequence[] = { 1, 2, 0, 3, 4, 5, 6, 7, 8, 9 };
        const int gearSequenceLen = int(sizeof(gearSequence) / sizeof(gearSequence[0]));
        int newGear = gearSequence[m_simGearCycleIndex];
        if (newGear != m_gear) { m_gear = newGear; emit gearChanged(); }
        m_simGearCycleIndex = (m_simGearCycleIndex + 1) % gearSequenceLen;
        m_simGearCycleTimer = 2.0;
    }

    double newRpm;
    if (m_gear < 3) {
        newRpm = 800.0 + rnd() * 100.0;
    } else {
        static const double ratios[] = { 0, 3.5, 2.5, 1.8, 1.4, 1.1, 0.9, 0.75 }; // [0] unused, [1..7]=1st..7th
        double base = m_speed * ratios[m_gear - 2] * 30.0;
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

    // Oil pressure: rises with rpm, same threshold decodeFrame() uses to
    // derive the warn boolean (rpm >= 600 && psi <= 40) — m_simOilFault
    // (flipped in the phase-random block above) forces a dip so the warn
    // icon can still be exercised without real CAN hardware.
    double targetOilPressure = 40.0 + (m_rpm / 7200.0) * 50.0;
    if (m_simOilFault) targetOilPressure *= 0.3;
    if (!qFuzzyCompare(1.0 + targetOilPressure, 1.0 + m_oilPressurePsi)) {
        m_oilPressurePsi = targetOilPressure;
        emit oilPressureChanged();
    }
    bool oilWarn = (m_rpm >= 600.0) && (m_oilPressurePsi <= 40.0);
    if (oilWarn != m_oilPressureWarn) { m_oilPressureWarn = oilWarn; emit oilPressureWarnChanged(); }

    // Battery voltage: steady alternator output, same 12.5V threshold
    // decodeFrame() uses — m_simBattFault forces a dip to exercise the warn
    // icon.
    double targetVbat = m_simBattFault ? 11.8 : 13.8;
    double newVbat = m_vbat + (targetVbat - m_vbat) * 0.1;
    if (!qFuzzyCompare(1.0 + newVbat, 1.0 + m_vbat)) { m_vbat = newVbat; emit vbatChanged(); }
    bool battWarn = m_vbat < 12.5;
    if (battWarn != m_batteryWarn) { m_batteryWarn = battWarn; emit batteryWarnChanged(); }
}
#endif

// Not simulate-only — see the Q_INVOKABLE declarations in canbus.h for why
// these must work on every build, real hardware included. Plain level
// flips, matching real CAN's DIN pass-through (decodeFrame()'s
// kMce18Base+2 case) — the visible blink is synthesized from the level in
// QML (main.qml's blinkTimer), not here.
void CanBus::debugToggleLeftIndicator()
{
    m_leftIndicator = !m_leftIndicator;
    emit leftIndicatorChanged();
}

void CanBus::debugToggleRightIndicator()
{
    m_rightIndicator = !m_rightIndicator;
    emit rightIndicatorChanged();
}

void CanBus::debugToggleHazard()
{
    m_hazard = !m_hazard;
    emit hazardChanged();
}

void CanBus::debugGearUp()
{
    int newGear = qBound(0, m_gear + 1, 9);
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    m_simGearManualOverride = true;
#endif
    if (newGear != m_gear) { m_gear = newGear; emit gearChanged(); }
}

void CanBus::debugGearDown()
{
    int newGear = qBound(0, m_gear - 1, 9);
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
    m_simGearManualOverride = true;
#endif
    if (newGear != m_gear) { m_gear = newGear; emit gearChanged(); }
}
