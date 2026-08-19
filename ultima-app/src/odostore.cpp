#include "odostore.h"
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <fcntl.h>
#include <unistd.h>

static const double DEFAULT_TOTAL_ODO = 0.0;
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

// QSaveFile writes to a temp file in the same directory and renames it over
// m_path on commit, so a power cut mid-write leaves the old odometer.json
// intact instead of a truncated/unparseable one (which load() would silently
// treat as "no data" and reset the odometer to the hardcoded default). The
// explicit fsyncs make the write and the rename itself survive a power cut
// immediately after a "successful" save, not just protect against a torn one.
void OdoStore::save()
{
    QJsonObject obj;
    obj["totalOdo"] = m_totalOdo;
    obj["tripOdo"] = m_tripOdo;

    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "OdoStore: failed to write %s\n", qPrintable(m_path));
        return;
    }

    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    f.flush();
    fsync(f.handle());

    if (!f.commit()) {
        fprintf(stderr, "OdoStore: failed to commit %s\n", qPrintable(m_path));
        return;
    }

    const QByteArray dirPath = QFileInfo(m_path).absolutePath().toLocal8Bit();
    int dirFd = ::open(dirPath.constData(), O_RDONLY);
    if (dirFd >= 0) {
        fsync(dirFd);
        ::close(dirFd);
    }

    fprintf(stderr, "OdoStore: saved totalOdo=%.1f tripOdo=%.1f\n", m_totalOdo, m_tripOdo);
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
