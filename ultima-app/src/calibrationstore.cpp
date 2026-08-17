#include "calibrationstore.h"

#include <QFile>
#include <QJsonDocument>
#include <cstdio>

CalibrationStore::CalibrationStore(const QString &path, QObject *parent)
    : QObject(parent), m_path(path)
{
    m_front = new CalibrationCameraModel(this);
    m_rear = new CalibrationCameraModel(this);
    m_left = new CalibrationCameraModel(this);
    m_right = new CalibrationCameraModel(this);
    m_geometry = new CalibrationGeometryModel(this);

    connect(m_front, &CalibrationCameraModel::changed, this, &CalibrationStore::calibrationChanged);
    connect(m_rear, &CalibrationCameraModel::changed, this, &CalibrationStore::calibrationChanged);
    connect(m_left, &CalibrationCameraModel::changed, this, &CalibrationStore::calibrationChanged);
    connect(m_right, &CalibrationCameraModel::changed, this, &CalibrationStore::calibrationChanged);
    connect(m_geometry, &CalibrationGeometryModel::changed, this, &CalibrationStore::calibrationChanged);

    load();
}

void CalibrationStore::applySet(const CalibrationSet &set)
{
    m_front->fromStruct(set.front);
    m_rear->fromStruct(set.rear);
    m_left->fromStruct(set.left);
    m_right->fromStruct(set.right);
    m_geometry->fromStruct(set.geometry);
}

void CalibrationStore::load()
{
    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            applySet(CalibrationSet::fromJson(doc.object()));
            return;
        }
        fprintf(stderr, "CalibrationStore: invalid JSON in %s: %s\n",
                qPrintable(m_path), qPrintable(err.errorString()));
    }
    applySet(defaultCalibration());
}

CalibrationSet CalibrationStore::toSet() const
{
    CalibrationSet set;
    set.front = m_front->toStruct();
    set.rear = m_rear->toStruct();
    set.left = m_left->toStruct();
    set.right = m_right->toStruct();
    set.geometry = m_geometry->toStruct();
    return set;
}

void CalibrationStore::save()
{
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        fprintf(stderr, "CalibrationStore: failed to write %s\n", qPrintable(m_path));
        return;
    }
    f.write(QJsonDocument(toSet().toJson()).toJson(QJsonDocument::Indented));
}

void CalibrationStore::resetToDefaults()
{
    applySet(defaultCalibration());
}
