#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // Read kernel uptime (seconds since power on)
    double bootTime = 0;
    QFile uptime("/proc/uptime");
    if (uptime.open(QIODevice::ReadOnly)) {
        bootTime = QString(uptime.readAll()).split(' ').first().toDouble();
        uptime.close();
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("bootTime", bootTime);
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
