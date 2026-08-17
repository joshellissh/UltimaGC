QT += qml quick
# QOpenGLShaderProgram/QOpenGLBuffer/QOpenGLVertexArrayObject/QOpenGLFunctions
# (warpmesh.cpp, shadermanager.cpp, surroundtexture.cpp, surroundview.cpp)
# live in QtGui itself on Qt5 (the target build) but moved into a separate
# Qt6::OpenGL module — only needed on the Homebrew Qt6 macOS dev build.
greaterThan(QT_MAJOR_VERSION, 5) {
    QT += opengl
}
CONFIG += c++17
# CONFIG += qtquickcompiler was tried here (2026-08-11) and reverted — not
# just "didn't help," actively broken on this Yocto build. meta-qt5's
# qtdeclarative-native here doesn't ship `qmlcachegen`
# (recipe-sysroot-native/usr/bin/qmlcachegen: not found), and rather than
# failing cleanly, qmake's qtquickcompiler feature got stuck re-invoking
# `qmake -o Makefile` in an infinite loop (log.do_compile hit 36MB and
# climbing before being killed) — never converges on its own, burns CPU
# indefinitely. Confirmed the mechanism itself is sound (works fine, real
# qmlcachegen invocations, on the macOS Qt6 Homebrew dev build — see
# scripts/dev-build.sh) — this is specific to what this Yocto layer's Qt5
# build was configured to produce. Fixing it for real would mean adding a
# PACKAGECONFIG to meta-qt5's qtdeclarative recipe to build qmlcachegen for
# -native, not attempted since the expected win here was already small
# (tens of ms — see NOTES.md "Boot-time investigation" Candidate 4) next to
# this newly-discovered cost/risk. If revisited, first confirm
# qmlcachegen actually gets produced in recipe-sysroot-native before
# re-enabling this — don't just flip the CONFIG line again.
TARGET = ultima-app
HEADERS += odostore.h canbus.h systemclock.h camerafeed.h cameraview.h \
    cameracalibration.h warpmesh.h shadermanager.h surroundtexture.h surroundview.h
SOURCES += main.cpp odostore.cpp canbus.cpp systemclock.cpp camerafeed.cpp cameraview.cpp \
    cameracalibration.cpp warpmesh.cpp shadermanager.cpp surroundtexture.cpp surroundview.cpp
RESOURCES += qml.qrc

# Off by default so the Buildroot/Pi build always uses real SocketCAN.
# Dev-build scripts pass CONFIG+=ultima_dev_sim to get simulated gauge data
# on Linux dev builds too (macOS dev builds already simulate unconditionally).
ultima_dev_sim {
    DEFINES += ULTIMA_SIMULATE
}
