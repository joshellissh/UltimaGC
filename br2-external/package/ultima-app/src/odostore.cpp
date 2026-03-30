#include "odostore.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

static const double DEFAULT_TOTAL_ODO = 2347.0;
static const double DEFAULT_TRIP_ODO = 0.0;

OdoStore::OdoStore(const QString &path, QObject *parent)
    : QObject(parent)
    , m_path(path)
    , m_totalOdo(DEFAULT_TOTAL_ODO)
    , m_tripOdo(DEFAULT_TRIP_ODO)
{
    load();
}

void OdoStore::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj.contains("totalOdo"))
            m_totalOdo = obj["totalOdo"].toDouble(DEFAULT_TOTAL_ODO);
        if (obj.contains("tripOdo"))
            m_tripOdo = obj["tripOdo"].toDouble(DEFAULT_TRIP_ODO);
    }
}

void OdoStore::save()
{
    QJsonObject obj;
    obj["totalOdo"] = m_totalOdo;
    obj["tripOdo"] = m_tripOdo;

    QFile f(m_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
        fprintf(stderr, "OdoStore: saved totalOdo=%.1f tripOdo=%.1f\n", m_totalOdo, m_tripOdo);
    } else {
        fprintf(stderr, "OdoStore: failed to write %s\n", qPrintable(m_path));
    }
}

void OdoStore::setTotalOdo(double v)
{
    if (m_totalOdo != v) {
        m_totalOdo = v;
        emit totalOdoChanged();
    }
}

void OdoStore::setTripOdo(double v)
{
    if (m_tripOdo != v) {
        m_tripOdo = v;
        emit tripOdoChanged();
    }
}
