import QtQuick 2.15

Item {
    id: engine

    property real speed: 0
    property real rpm: 0
    property real fuelConsumption: 3.5
    property int gear: -2  // -2=P, -1=R, 0=N, 1-7=forward
    property real totalOdo: odoStore.totalOdo
    property real tripOdo: odoStore.tripOdo
    property real outsideTemp: 14.5
    property string driveMode: "ECO PRO"
    property real fuelLevel: 0.7
    property real coolantTemp: 190
    property bool leftIndicator: false
    property bool rightIndicator: false
    property bool lowBeams: false
    property bool highBeams: false
    property bool oilPressureWarn: false
    property bool checkEngine: false
    property bool batteryWarn: false
    property bool coolantWarn: false
    property real boost: 0  // 0-30 PSI

    // Internal state
    property real _targetSpeed: 0
    property real _phaseTimer: 0
    property real _accel: 0

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            engine.leftIndicator = !engine.leftIndicator
            engine.rightIndicator = !engine.rightIndicator
        }
    }

    Timer {
        interval: 60
        running: true
        repeat: true
        onTriggered: {
            var dt = interval / 1000.0

            // Phase cycling: pick new target speeds periodically
            engine._phaseTimer -= dt
            if (engine._phaseTimer <= 0) {
                var phases = [
                    { target: 30 + Math.random() * 20, dur: 8 + Math.random() * 6 },   // city
                    { target: 0,                         dur: 3 + Math.random() * 3 },   // stop
                    { target: 60 + Math.random() * 30,  dur: 10 + Math.random() * 8 },  // suburban
                    { target: 100 + Math.random() * 40, dur: 12 + Math.random() * 10 }, // highway
                    { target: 120 + Math.random() * 60, dur: 8 + Math.random() * 6 },   // spirited
                ]
                var phase = phases[Math.floor(Math.random() * phases.length)]
                engine._targetSpeed = phase.target
                engine._phaseTimer = phase.dur

                // Toggle warning indicators on phase change
                engine.highBeams = engine.speed > 50 && Math.random() < 0.3
                if (Math.random() < 0.3) engine.oilPressureWarn = !engine.oilPressureWarn
                if (Math.random() < 0.4) engine.checkEngine = !engine.checkEngine
                if (Math.random() < 0.3) engine.batteryWarn = !engine.batteryWarn
                if (Math.random() < 0.15) engine.coolantWarn = !engine.coolantWarn
            }

            // Smooth approach to target + slight noise
            engine._accel = (engine._targetSpeed - engine.speed) * 0.02
            var noise = (Math.random() - 0.5) * 0.8
            engine.speed = Math.max(0, Math.min(220, engine.speed + engine._accel + noise))

            // Fuel consumption: higher when accelerating hard, lower when cruising
            var accelFactor = Math.abs(engine._accel) * 15
            var speedFactor = engine.speed > 5 ? (4.0 + engine.speed * 0.06) : 0
            var targetFuel = speedFactor + accelFactor + (Math.random() - 0.5) * 1.5
            engine.fuelConsumption += (Math.max(0, Math.min(25, targetFuel)) - engine.fuelConsumption) * 0.08

            // Gear from speed brackets (-2=P, -1=R, 0=N, 1-7=forward)
            if (engine.speed < 3) {
                // When stopped, cycle through P/R/N
                if (engine._targetSpeed === 0) {
                    var stopGears = [-2, -2, -2, 0, -1]  // mostly P, sometimes N or R
                    if (engine.gear > 0) engine.gear = 0  // downshift to N first
                    else if (engine._phaseTimer < 1) engine.gear = stopGears[Math.floor(Math.random() * stopGears.length)]
                } else {
                    engine.gear = 0
                }
            }
            else if (engine.speed < 20) engine.gear = 1
            else if (engine.speed < 40) engine.gear = 2
            else if (engine.speed < 65) engine.gear = 3
            else if (engine.speed < 100) engine.gear = 4
            else if (engine.speed < 140) engine.gear = 5
            else if (engine.speed < 170) engine.gear = 6
            else engine.gear = 7

            // RPM derived from speed and gear
            if (engine.gear <= 0) {
                engine.rpm = 800 + Math.random() * 100  // idle / P / R / N
            } else {
                var gearRatios = [0, 3.5, 2.5, 1.8, 1.4, 1.1, 0.9, 0.75]
                var baseRpm = engine.speed * gearRatios[engine.gear] * 30
                engine.rpm = Math.max(800, Math.min(8000, baseRpm + (Math.random() - 0.5) * 200))
            }

            // Trip odo increments based on speed (km/h -> km per tick)
            engine.tripOdo += engine.speed * dt / 3600.0
            engine.totalOdo += engine.speed * dt / 3600.0

            // Slight outside temp drift
            engine.outsideTemp += (Math.random() - 0.5) * 0.02

            // Fuel slowly decreases
            engine.fuelLevel = Math.max(0, engine.fuelLevel - engine.speed * dt / 360000.0)

            // Coolant temp settles around 190-200, rises with RPM
            var targetCoolant = 185 + engine.rpm / 2000 * 15 + (Math.random() - 0.5) * 2
            engine.coolantTemp += (targetCoolant - engine.coolantTemp) * 0.01

            // Boost: builds with throttle at higher RPM
            var boostTarget = 0
            if (engine.speed > 20 && engine._accel > 0) {
                boostTarget = Math.min(30, engine._accel * 200 + engine.rpm / 300)
            }
            engine.boost += (boostTarget - engine.boost) * 0.05

            // Dashboard indicators
            engine.lowBeams = engine.speed > 5
            engine.coolantWarn = engine.coolantTemp > 220
        }
    }
}
