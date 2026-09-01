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
#include <QQuickImageProvider>
#include <QElapsedTimer>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <future>
#include <memory>
#include <signal.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSGRendererInterface>
#endif

#include "odostore.h"
#include "canbus.h"
#include "systemclock.h"
#include "systemstats.h"
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

// sd_notify(3) without libsystemd: one datagram to $NOTIFY_SOCKET. The
// BeagleY-AI unit is Type=notify and sends READY=1 from the first rendered
// frame (see logFirstFrame below), which is what lets the rest of the boot
// (udev coldplug, the ~50 module loads it triggers, remoteproc firmware,
// networking) be ordered *after* the dash is on screen instead of competing
// with it for the SD card and the cores — see
// beagley-ai/meta-ultima-beagley-ai-src/recipes-ultima/ultima-app/. A
// no-op when NOTIFY_SOCKET is unset (dev builds, or a
// Type=simple unit), so it costs nothing anywhere else.
static void notifySystemd(const char *state)
{
#if defined(__linux__)
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return;
    const size_t len = strlen(path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (len >= sizeof(addr.sun_path))
        return;
    memcpy(addr.sun_path, path, len + 1);
    socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + len + 1;
    if (path[0] == '@') {            // abstract namespace socket
        addr.sun_path[0] = '\0';
        addrLen = offsetof(struct sockaddr_un, sun_path) + len;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sendto(fd, state, strlen(state), MSG_NOSIGNAL, reinterpret_cast<struct sockaddr *>(&addr), addrLen);
    close(fd);
#else
    (void)state;
#endif
}

// The two splash bitmaps main.qml's intro overlay shows on the first frame
// (image://splash/screen, image://splash/car), decoded on their own threads
// from the moment main() starts. Rationale (BeagleY-AI, 2026-08-30): Qt's
// asynchronous Image path decodes on a single reader thread, in order, and
// these two 1600x720 / 1104x364 PNGs took ~0.6 s there — longer than the
// rest of the QML took to instantiate, so the window (held until they are
// ready, see main.qml) sat waiting. Started here they overlap the ~0.4 s of
// KMS/EGL setup inside QGuiApplication's constructor, during which three of
// the four cores are idle, and are done before the QML engine even exists.
// QImage needs no QGuiApplication; qrc is registered by a static initialiser.
class SplashImageProvider : public QQuickImageProvider
{
public:
    SplashImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_screen(std::async(std::launch::async, [] { return QImage(QStringLiteral(":/splash_screen.png")); }))
        , m_car(std::async(std::launch::async, [] { return QImage(QStringLiteral(":/splash_car_start.png")); }))
    {}
    QImage requestImage(const QString &id, QSize *size, const QSize &) override
    {
        QImage img;
        if (id == QLatin1String("screen"))
            img = m_screen.get();
        else if (id == QLatin1String("car"))
            img = m_car.get();
        if (size)
            *size = img.size();
        return img;
    }
private:
    std::shared_future<QImage> m_screen;
    std::shared_future<QImage> m_car;
};

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

    // First thing, before QGuiApplication: kicks off the splash decodes
    // (see the class). Ownership passes to the QML engine below.
    auto *splashProvider = new SplashImageProvider;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6 defaults to Metal on macOS (and other non-OpenGL RHI backends
    // elsewhere); SurroundView (a QQuickFramebufferObject, see
    // surroundview.h) only renders anything when the scene graph runs on
    // the OpenGL RHI backend — under Metal its Renderer::render() is never
    // invoked, silently leaving the item blank with no warning. Must be set
    // before the first QQuickWindow is created. The target's Qt5
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
    // hardware bring-up — see beagley-ai/NOTES.md), so it changes
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
    SystemStats systemStats;

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
    {
        double t = readUptime();
        fprintf(stderr, "[%6.2f] core objects created (+%.2fs)\n", t, t-t1);
    }

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
    engine.addImageProvider(QStringLiteral("splash"), splashProvider);
    engine.rootContext()->setContextProperty("bootTime", t0);
    engine.rootContext()->setContextProperty("odoStore", &odoStore);
    engine.rootContext()->setContextProperty("calibrationStore", &calibrationStore);
    engine.rootContext()->setContextProperty("sim", &canBus);
    engine.rootContext()->setContextProperty("systemClock", &systemClock);
    engine.rootContext()->setContextProperty("sysStats", &systemStats);
    engine.rootContext()->setContextProperty("cameraFeed1", &cameraFeed1);
    // EXPERIMENT (2026-08-26): ULTIMA_CAM_FANOUT=1 points cameraFeed2..4 at
    // cameraFeed1's object, so every grid quadrant renders the one attached
    // camera — a 4-camera render-thread/upload load proxy for a bench with a
    // single camera. Not a shipping mode.
    const bool camFanout = qEnvironmentVariableIntValue("ULTIMA_CAM_FANOUT") != 0;
    engine.rootContext()->setContextProperty("cameraFeed2", camFanout ? &cameraFeed1 : &cameraFeed2);
    engine.rootContext()->setContextProperty("cameraFeed3", camFanout ? &cameraFeed1 : &cameraFeed3);
    engine.rootContext()->setContextProperty("cameraFeed4", camFanout ? &cameraFeed1 : &cameraFeed4);
    engine.rootContext()->setContextProperty("splashImagePath", splashImageUrl);
    // Debug: ULTIMA_DEFAULT_CAMERA=grid (or "360") makes the app boot straight
    // into that camera overlay instead of the plain gauge cluster — for
    // bench-watching the camera (e.g. the N4 lock/warp) without triggering it
    // by hand each boot. Empty/unset (the default) = normal dash boot, so this
    // is production-safe and opt-in. main.qml opens the screen once the window
    // is shown (see its onVisibleChanged).
    engine.rootContext()->setContextProperty(
        "defaultCameraScreen", qEnvironmentVariable("ULTIMA_DEFAULT_CAMERA"));
    {
        double t = readUptime();
        fprintf(stderr, "[%6.2f] QML engine ready (+%.2fs)\n", t, t-t1);
    }
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
    // boot appends and still lands on the serial console for a
    // serial boot-timing capture to pick up in the same timeline as power-on).
    auto rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    std::atomic<bool> renderedOnce{false};
    std::atomic<bool> swappedOnce{false};
    auto logFirstFrame = [t0](std::atomic<bool> &done, const char *label) -> bool {
        if (bool expected = false; !done.compare_exchange_strong(expected, true))
            return false;
        double t = readUptime();
        fprintf(stderr, "[%6.2f] %s (+%.2fs since app main())\n", t, label, t-t0);
        if (FILE *kmsg = fopen("/dev/kmsg", "w")) {
            fprintf(kmsg, "<3>ultima-app: [%.2f] %s\n", t, label);
            fclose(kmsg);
        }
        return true;
    };
    if (rootWindow) {
        QObject::connect(rootWindow, &QQuickWindow::afterRendering, rootWindow, [&]() {
            // The dash is on screen: tell systemd (Type=notify units only)
            // so everything ordered After=ultima-app.service can start now.
            if (logFirstFrame(renderedOnce, "first frame rendered (afterRendering)"))
                notifySystemd("READY=1\nSTATUS=first frame rendered");
        }, Qt::DirectConnection);
        QObject::connect(rootWindow, &QQuickWindow::frameSwapped, rootWindow, [&]() {
            logFirstFrame(swappedOnce, "first frame swapped (frameSwapped)");
        }, Qt::DirectConnection);

        // main.qml's Window is `visible: false`; it's shown from here, once
        // the two splash bitmaps it decodes asynchronously are ready (its
        // introAssetsReady property — see the comment on the Window), so
        // that (a) the hooks above exist before the first frame is drawn,
        // and (b) that first frame still shows the splash rather than a
        // black window. On eglfs show() renders synchronously, so the
        // "first frame" marks fire inside it. If the assets aren't ready
        // straight away, poll briefly; past the deadline show anyway — a
        // missing PNG must never keep the dash black.
        auto showRootWindow = [rootWindow, t0](const char *why) {
            if (rootWindow->isVisible())
                return;
            double t = readUptime();
            fprintf(stderr, "[%6.2f] showing window (%s, +%.2fs since app main())\n", t, why, t-t0);
            rootWindow->show();
        };
        if (rootWindow->property("introAssetsReady").toBool()) {
            showRootWindow("intro assets ready");
        } else {
            auto *showTimer = new QTimer(&app);
            auto deadline = std::make_shared<QElapsedTimer>();
            deadline->start();
            QObject::connect(showTimer, &QTimer::timeout, rootWindow, [=]() {
                if (rootWindow->property("introAssetsReady").toBool())
                    showRootWindow("intro assets ready");
                else if (deadline->elapsed() >= 1500)
                    showRootWindow("deadline, assets not ready");
                else
                    return;
                showTimer->stop();
                showTimer->deleteLater();
            });
            showTimer->start(5);
        }

        // Debug-only on-device screenshot capture. eglfs_kms has no
        // screenshot tooling on this image (no modetest/ffmpeg/fbgrab), and
        // /dev/fb0 doesn't reflect live GPU output once Qt takes over (see
        // GAUGE-CLUSTER.md's "Boot splash" section) — so this
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
