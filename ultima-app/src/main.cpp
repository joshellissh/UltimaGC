#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QMetaObject>
#include <QVariant>
#include <QSurfaceFormat>
#include <atomic>
#include <signal.h>
#include <unistd.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSGRendererInterface>
#endif

#include "odostore.h"
#include "canbus.h"
#include "systemclock.h"
#include "bluetoothmanager.h"
#include "camerafeed.h"
#include "cameraview.h"
#include "surroundview.h"

static double readUptime() {
    double t = 0;
    QFile f("/proc/uptime");
    if (f.open(QIODevice::ReadOnly)) {
        t = QString(f.readAll()).split(' ').first().toDouble();
        f.close();
    }
    return t;
}

static OdoStore *g_odoStore = nullptr;
static CanBus *g_canBus = nullptr;

static void sigHandler(int) {
    if (g_canBus)
        g_canBus->save();        // pushes latest odometer into OdoStore + persists
    else if (g_odoStore)
        g_odoStore->save();
    _exit(0);
}

int main(int argc, char *argv[])
{
    double t0 = readUptime();
    fprintf(stderr, "[%6.2f] app main() entered\n", t0);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6 defaults to Metal on macOS (and other non-OpenGL RHI backends
    // elsewhere); SurroundView (a QQuickFramebufferObject, see
    // surroundview.h) only renders anything when the scene graph runs on
    // the OpenGL RHI backend — under Metal its Renderer::render() is never
    // invoked, silently leaving the item blank with no warning. Must be set
    // before the first QQuickWindow is created. The BeaglePlay target's Qt5
    // build has no RHI concept at all (always real GL/GLES via eglfs), so
    // this is a macOS/Qt6-dev-build-only concern — ported from the same fix
    // in test/avm-benchmark/src/main.cpp, which hit this identical failure
    // mode with its own QQuickFramebufferObject-based dashboard item.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
    // QQuickWindow creates its GL context from QSurfaceFormat::defaultFormat(),
    // which defaults to a legacy/unspecified-version compatibility profile
    // on macOS (grants OpenGL 2.1) — incompatible with shadermanager.cpp's
    // "#version 330 core" header for desktop GL. Must be set before the
    // first window. On Linux/eglfs this states explicitly what's already
    // implicitly granted (GLES 3.1, confirmed via the PowerVR/mesa-pvr
    // hardware bring-up — see beagleplay-falcon/NOTES.md), so it changes
    // nothing there; it's load-bearing only for the macOS dev build.
    QSurfaceFormat surroundFormat;
#if defined(__linux__)
    surroundFormat.setRenderableType(QSurfaceFormat::OpenGLES);
    surroundFormat.setVersion(3, 1);
#else
    surroundFormat.setRenderableType(QSurfaceFormat::OpenGL);
    surroundFormat.setProfile(QSurfaceFormat::CoreProfile);
    surroundFormat.setVersion(3, 3);
#endif
    QSurfaceFormat::setDefaultFormat(surroundFormat);

    QGuiApplication app(argc, argv);
    double t1 = readUptime();
    fprintf(stderr, "[%6.2f] QGuiApplication created (+%.2fs)\n", t1, t1-t0);

    OdoStore odoStore("/data/odometer.json");
    g_odoStore = &odoStore;

    // Same /data persistence pattern as OdoStore above — see calibrationstore.h.
    CalibrationStore calibrationStore("/data/calibration.json");

    CanBus canBus(&odoStore);
    g_canBus = &canBus;

    SystemClock systemClock;
    BluetoothManager bluetoothManager;
#if defined(__linux__)
    // Always-on: the dash should be discoverable/connectable over BLE
    // regardless of whether anyone has opened BluetoothScreen (see
    // ANDROID-BLE-INTEGRATION.md, which assumes a companion app can connect
    // at any time — e.g. WiFi provisioning before getting in the car — not
    // only while someone is standing at the touchscreen with that screen
    // open). Linux-only: merely constructing the Bluetooth objects fires a
    // CoreBluetooth permission prompt at process launch on the macOS dev
    // build (see bluetoothmanager.h) — no reason to impose that on every
    // local QML-iteration run, so that build stays fully lazy.
    //
    // Deferred via singleShot(0), not called directly here — hardware-
    // confirmed 2026-08-19: a direct call at this point runs before
    // app.exec() ever starts the event loop, and QBluetoothLocalDevice's
    // BlueZ backend never recovers even long after bluetoothd comes up
    // afterward (a startAdvertising() retry loop kept reconstructing the
    // object and re-checking isValid() every 1s for 40+ seconds past
    // bluetoothd's startup, still failing — vs. the exact same code called
    // later from a QML slot, well inside a running event loop, which
    // worked immediately). Most likely cause: QDBusConnection::systemBus()
    // is a process-wide singleton Qt sets up lazily on first use, and
    // setting it up with no event loop pumping messages yet appears to
    // wedge it permanently for the rest of the process — a fresh
    // QBluetoothLocalDevice still inherits that same broken shared
    // connection, so reconstructing the object doesn't help. Deferring
    // this call to fire only once the event loop is already running
    // avoids ever touching D-Bus in that broken pre-exec() state.
    QTimer::singleShot(0, &bluetoothManager, &BluetoothManager::startAdvertising);
#endif

    // 4 independent feeds, one per /dev/mycam/camN — the mycam004m driver's
    // stable symlinks over whichever backend (fake or real) is currently
    // selected via its own select-camera-backend.sh, run outside this app
    // (see ~/code/mycam004m/docs/ultima-app-integration.md). Devices are
    // not opened here — CameraFeed stays lazy (see camerafeed.h) until
    // Camera360Screen actually opens, so a screen most drives never visit
    // costs nothing at boot.
    CameraFeed cameraFeed1("/dev/mycam/cam1");
    CameraFeed cameraFeed2("/dev/mycam/cam2");
    CameraFeed cameraFeed3("/dev/mycam/cam3");
    CameraFeed cameraFeed4("/dev/mycam/cam4");
    qmlRegisterType<CameraView>("Ultima", 1, 0, "CameraView");
    qmlRegisterType<SurroundView>("Ultima", 1, 0, "SurroundView");

    signal(SIGTERM, sigHandler);
    signal(SIGINT, sigHandler);

    // Dev-only boot splash simulation (scripts/dev-build.sh --boot): points
    // at the real on-target splash art so the whole boot flow — splash then
    // hard cut to the gauge cluster's own startup sweep — can be screen-
    // recorded without hardware. Empty/unset (the default) means main.qml's
    // splash overlay never appears, so plain `dev-build.sh` is unaffected.
    QString splashImageFile = qEnvironmentVariable("ULTIMA_SPLASH_IMAGE");
    QString splashImageUrl = splashImageFile.isEmpty()
        ? QString()
        : QUrl::fromLocalFile(splashImageFile).toString();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bootTime", t0);
    engine.rootContext()->setContextProperty("odoStore", &odoStore);
    engine.rootContext()->setContextProperty("calibrationStore", &calibrationStore);
    engine.rootContext()->setContextProperty("sim", &canBus);
    engine.rootContext()->setContextProperty("systemClock", &systemClock);
    engine.rootContext()->setContextProperty("bluetooth", &bluetoothManager);
    engine.rootContext()->setContextProperty("cameraFeed1", &cameraFeed1);
    engine.rootContext()->setContextProperty("cameraFeed2", &cameraFeed2);
    engine.rootContext()->setContextProperty("cameraFeed3", &cameraFeed3);
    engine.rootContext()->setContextProperty("cameraFeed4", &cameraFeed4);
    engine.rootContext()->setContextProperty("splashImagePath", splashImageUrl);
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    double t2 = readUptime();
    fprintf(stderr, "[%6.2f] QML loaded (+%.2fs)\n", t2, t2-t1);

    if (engine.rootObjects().isEmpty())
        return -1;

    double t3 = readUptime();
    fprintf(stderr, "[%6.2f] ready to render (+%.2fs)\n", t3, t3-t2);
    fprintf(stderr, "[%6.2f] total app startup: %.2fs\n", t3, t3-t0);

    // "ready to render" above is just QML component construction, before
    // app.exec() even starts the event loop — it's not a frame. These hooks
    // catch the actual first paint, logged to both the journal (stderr) and
    // /dev/kmsg (level 3, so it survives the `quiet` kernel cmdline falcon
    // boot appends and still lands on the serial console for
    // measure-boot.sh to pick up in the same timeline as power-on).
    auto rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    std::atomic<bool> renderedOnce{false};
    std::atomic<bool> swappedOnce{false};
    auto logFirstFrame = [t0](std::atomic<bool> &done, const char *label) {
        if (bool expected = false; !done.compare_exchange_strong(expected, true))
            return;
        double t = readUptime();
        fprintf(stderr, "[%6.2f] %s (+%.2fs since app main())\n", t, label, t-t0);
        if (FILE *kmsg = fopen("/dev/kmsg", "w")) {
            fprintf(kmsg, "<3>ultima-app: [%.2f] %s\n", t, label);
            fclose(kmsg);
        }
    };
    if (rootWindow) {
        QObject::connect(rootWindow, &QQuickWindow::afterRendering, rootWindow, [&]() {
            logFirstFrame(renderedOnce, "first frame rendered (afterRendering)");
        }, Qt::DirectConnection);
        QObject::connect(rootWindow, &QQuickWindow::frameSwapped, rootWindow, [&]() {
            logFirstFrame(swappedOnce, "first frame swapped (frameSwapped)");
        }, Qt::DirectConnection);

        // Debug-only on-device screenshot capture. eglfs_kms has no
        // screenshot tooling on this image (no modetest/ffmpeg/fbgrab), and
        // /dev/fb0 doesn't reflect live GPU output once Qt takes over (see
        // beagleplay-falcon/NOTES.md's boot-splash investigation) — so this
        // is the only way to pull a frame off real hardware short of
        // photographing the panel. Triggered by touching
        // /tmp/ultima-screenshot.request (optionally containing an output
        // path) over ssh; polled on a QTimer rather than a signal handler
        // since QQuickWindow::grabWindow() must run on the GUI thread.
        auto *screenshotTimer = new QTimer(&app);
        QObject::connect(screenshotTimer, &QTimer::timeout, rootWindow, [rootWindow]() {
            QFile trigger(QStringLiteral("/tmp/ultima-screenshot.request"));
            if (!trigger.exists())
                return;
            QString outPath = QStringLiteral("/tmp/ultima-screenshot.png");
            if (trigger.open(QIODevice::ReadOnly)) {
                QString requested = QString::fromUtf8(trigger.readAll()).trimmed();
                if (!requested.isEmpty())
                    outPath = requested;
                trigger.close();
            }
            QFile::remove(QStringLiteral("/tmp/ultima-screenshot.request"));
            QImage img = rootWindow->grabWindow();
            if (img.save(outPath))
                fprintf(stderr, "[screenshot] saved %s (%dx%d)\n", qPrintable(outPath), img.width(), img.height());
            else
                fprintf(stderr, "[screenshot] failed to save %s\n", qPrintable(outPath));
        });
        screenshotTimer->start(250);

        // Debug-only: drives CameraGridScreen/Camera360Screen open/closed
        // from /tmp/ultima-camtest.request ("open"/"close"/"360open"/
        // "360close") — same polled-file pattern as the screenshot trigger
        // above, needed because this board has no touchscreen-input-
        // injection tool over SSH (see main.qml's debugSetCameraGrid()/
        // debugSetCamera360()).
        auto *camTestTimer = new QTimer(&app);
        QObject::connect(camTestTimer, &QTimer::timeout, rootWindow, [rootWindow]() {
            QFile trigger(QStringLiteral("/tmp/ultima-camtest.request"));
            if (!trigger.exists())
                return;
            QString cmd;
            if (trigger.open(QIODevice::ReadOnly)) {
                cmd = QString::fromUtf8(trigger.readAll()).trimmed();
                trigger.close();
            }
            QFile::remove(QStringLiteral("/tmp/ultima-camtest.request"));
            if (cmd == QStringLiteral("360open") || cmd == QStringLiteral("360close")) {
                QMetaObject::invokeMethod(rootWindow, "debugSetCamera360",
                                           Q_ARG(QVariant, cmd == QStringLiteral("360open")));
            } else {
                QMetaObject::invokeMethod(rootWindow, "debugSetCameraGrid",
                                           Q_ARG(QVariant, cmd == QStringLiteral("open")));
            }
        });
        camTestTimer->start(250);

        // Debug-only: toggles turn-signal/hazard indicators from
        // /tmp/ultima-indicator.request ("left"/"right"/"hazard") — same
        // polled-file pattern as the screenshot/camtest triggers above,
        // needed because this board has no L/R/H key to press over SSH
        // (see main.qml's Keys.onPressed). These are the same
        // Q_INVOKABLE toggles that key handler calls, so a second
        // trigger with the same command toggles it back off, just like a
        // second keypress would.
        auto *indicatorTestTimer = new QTimer(&app);
        QObject::connect(indicatorTestTimer, &QTimer::timeout, &canBus, [&canBus]() {
            QFile trigger(QStringLiteral("/tmp/ultima-indicator.request"));
            if (!trigger.exists())
                return;
            QString cmd;
            if (trigger.open(QIODevice::ReadOnly)) {
                cmd = QString::fromUtf8(trigger.readAll()).trimmed();
                trigger.close();
            }
            QFile::remove(QStringLiteral("/tmp/ultima-indicator.request"));
            if (cmd == QStringLiteral("left"))
                canBus.debugToggleLeftIndicator();
            else if (cmd == QStringLiteral("right"))
                canBus.debugToggleRightIndicator();
            else if (cmd == QStringLiteral("hazard"))
                canBus.debugToggleHazard();
        });
        indicatorTestTimer->start(250);
    }

    return app.exec();
}
