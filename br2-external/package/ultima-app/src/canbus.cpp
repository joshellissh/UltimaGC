#include "canbus.h"
#include "odostore.h"

#include <QDateTime>
#include <QtGlobal>
#if !defined(__linux__) || defined(ULTIMA_SIMULATE)
#include <QRandomGenerator>
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
    case 0x600: {                                       // Frame 1: rpm @ slot 1
        double v = qMax(0, int(be_s16(d, 0)));
        if (v != m_rpm) { m_rpm = v; emit rpmChanged(); }
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
    case 0x605: {                                       // Frame 6: ect1 @ slot 2
        double c = be_s16(d, 2) * 0.1;
        double f = c * 1.8 + 32.0;
        if (!qFuzzyCompare(1.0 + f, 1.0 + m_coolantTempF)) {
            m_coolantTempF = f;
            emit coolantTempChanged();
        }
        bool warn = c > 110.0;                          // ~230 °F
        if (warn != m_coolantWarn) { m_coolantWarn = warn; emit coolantWarnChanged(); }
        break;
    }
    case 0x608: {                                       // Frame 9: eop1 @ slot 1
        double eopKpa = be_s16(d, 0) * 0.1;
        // Warn only when engine is actually running — at crank/idle pressure
        // can momentarily dip without indicating a fault.
        bool warn = (m_rpm > 1200.0) && (eopKpa < 100.0);
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
        bool warn = vbat > 0.5 && vbat < 12.0;          // ignore powered-off readings
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
    case 0x614: {                                       // Frame 21: mapMax @ slot 1
        // MAP is absolute pressure (kPa); convert to boost in psi relative to
        // atmospheric. Clamp at 0 — under vacuum we just show "no boost".
        double mapKpa = be_s16(d, 0) * 0.1;
        double psi = qMax(0.0, (mapKpa - 101.325) * 0.145038);
        if (!qFuzzyCompare(1.0 + psi, 1.0 + m_boostPsi)) {
            m_boostPsi = psi;
            emit boostChanged();
        }
        break;
    }
    // Still not broadcast on this CAN2 config:
    //   flvlA — fuel gauge stays at 0 until added to SCal
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
    auto rnd = []() { return QRandomGenerator::global()->generateDouble(); };

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

    int newGear;
    if      (m_speed < 3.0)   newGear = 0;
    else if (m_speed < 12.0)  newGear = 1;
    else if (m_speed < 25.0)  newGear = 2;
    else if (m_speed < 40.0)  newGear = 3;
    else if (m_speed < 62.0)  newGear = 4;
    else if (m_speed < 87.0)  newGear = 5;
    else if (m_speed < 106.0) newGear = 6;
    else if (m_speed < 137.0) newGear = 7;
    else                      newGear = 8;
    if (newGear != m_gear) { m_gear = newGear; emit gearChanged(); }

    double newRpm;
    if (m_gear <= 0) {
        newRpm = 800.0 + rnd() * 100.0;
    } else {
        static const double ratios[] = { 0, 3.5, 2.5, 1.8, 1.4, 1.1, 0.9, 0.75, 0.65 };
        double base = m_speed * ratios[m_gear] * 30.0;
        newRpm = qBound(800.0, base + (rnd() - 0.5) * 200.0, 7200.0);
    }
    if (!qFuzzyCompare(1.0 + newRpm, 1.0 + m_rpm)) { m_rpm = newRpm; emit rpmChanged(); }

    double targetCoolant = 190.0 + m_rpm / 2000.0 * 15.0 + (rnd() - 0.5) * 2.0;
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
}
#endif
