import QtQuick 2.15

Item {
    id: engine

    property real speed: 0
    property real rpm: 0
    property real fuelConsumption: 3.5
    property int gear: 0
    property real totalOdo: 2347.0
    property real tripOdo: 0.0
    property real outsideTemp: 14.5
    property string driveMode: "ECO PRO"
    property real fuelLevel: 0.7
    property real coolantTemp: 190
    property bool leftIndicator: false
    property bool rightIndicator: false

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

            // Gear from speed brackets
            if (engine.speed < 3) engine.gear = 0       // neutral
            else if (engine.speed < 20) engine.gear = 1
            else if (engine.speed < 40) engine.gear = 2
            else if (engine.speed < 65) engine.gear = 3
            else if (engine.speed < 100) engine.gear = 4
            else if (engine.speed < 140) engine.gear = 5
            else engine.gear = 6

            // RPM derived from speed and gear
            if (engine.gear === 0) {
                engine.rpm = 800 + Math.random() * 100  // idle
            } else {
                var gearRatios = [0, 3.5, 2.5, 1.8, 1.4, 1.1, 0.9]
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
        }
    }
}
