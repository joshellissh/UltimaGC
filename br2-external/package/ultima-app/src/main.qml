import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: root
    width: 1600
    height: 720
    visibility: Window.Windowed
    color: "black"

    SimEngine {
        id: sim
    }

    // Left gauge: Speedometer
    CircularGauge {
        id: speedGauge
        x: 50
        y: 30
        width: 600
        height: 600
        value: sim.speed
        minValue: 0
        maxValue: 220
        arcColor: "#FF8C00"
        needleColor: "#FF8C00"
        unitLabel: "km/h"
        warningStart: 180
        majorTicks: [0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220]
        minorTickCount: 3
    }

    // Center decorative element
    CenterLine {
        x: 550
        y: 60
        width: 500
        height: 560
    }

    // Right gauge: Fuel consumption
    CircularGauge {
        id: fuelGauge
        x: 950
        y: 30
        width: 600
        height: 600
        value: sim.fuelConsumption
        minValue: 0
        maxValue: 25
        arcColor: "#00CED1"
        needleColor: "#00CED1"
        unitLabel: "l/100km"
        majorTicks: [0, 5, 10, 15, 20, 25]
        minorTickCount: 4
    }

    // Bottom info bar
    InfoBar {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 50
        speed: sim.speed
        fuelConsumption: sim.fuelConsumption
        gear: sim.gear
        totalOdo: sim.totalOdo
        tripOdo: sim.tripOdo
        outsideTemp: sim.outsideTemp
        driveMode: sim.driveMode
    }
}
