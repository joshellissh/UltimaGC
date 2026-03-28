#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>

static double readUptime() {
    double t = 0;
    QFile f("/proc/uptime");
    if (f.open(QIODevice::ReadOnly)) {
        t = QString(f.readAll()).split(' ').first().toDouble();
        f.close();
    }
    return t;
}

int main(int argc, char *argv[])
{
    double t0 = readUptime();
    fprintf(stderr, "[%6.2f] app main() entered\n", t0);

    QGuiApplication app(argc, argv);
    double t1 = readUptime();
    fprintf(stderr, "[%6.2f] QGuiApplication created (+%.2fs)\n", t1, t1-t0);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bootTime", t0);
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    double t2 = readUptime();
    fprintf(stderr, "[%6.2f] QML loaded (+%.2fs)\n", t2, t2-t1);

    if (engine.rootObjects().isEmpty())
        return -1;

    double t3 = readUptime();
    fprintf(stderr, "[%6.2f] ready to render (+%.2fs)\n", t3, t3-t2);
    fprintf(stderr, "[%6.2f] total app startup: %.2fs\n", t3, t3-t0);

    return app.exec();
}
