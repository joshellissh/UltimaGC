#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <atomic>
#include <signal.h>
#include <unistd.h>

#include "odostore.h"
#include "canbus.h"
#include "systemclock.h"

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

    QGuiApplication app(argc, argv);
    double t1 = readUptime();
    fprintf(stderr, "[%6.2f] QGuiApplication created (+%.2fs)\n", t1, t1-t0);

    OdoStore odoStore("/data/odometer.json");
    g_odoStore = &odoStore;

    CanBus canBus(&odoStore);
    g_canBus = &canBus;

    SystemClock systemClock;

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
    engine.rootContext()->setContextProperty("sim", &canBus);
    engine.rootContext()->setContextProperty("systemClock", &systemClock);
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
    }

    return app.exec();
}
