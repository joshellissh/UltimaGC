QT += qml quick
CONFIG += c++17
TARGET = ultima-app
HEADERS += odostore.h canbus.h
SOURCES += main.cpp odostore.cpp canbus.cpp
RESOURCES += qml.qrc

# Off by default so the Buildroot/Pi build always uses real SocketCAN.
# Dev-build scripts pass CONFIG+=ultima_dev_sim to get simulated gauge data
# on Linux dev builds too (macOS dev builds already simulate unconditionally).
ultima_dev_sim {
    DEFINES += ULTIMA_SIMULATE
}
