#ifndef CALIBRATIONSTORE_H
#define CALIBRATIONSTORE_H

#include <QObject>
#include <QString>

#include "cameracalibration.h"

// QML-facing wrapper around one CameraCalibration — one instance per
// camera, owned by CalibrationStore. Every setter emits changed(), which
// CalibrationStore aggregates into its own calibrationChanged() signal for
// SurroundView to rebuild the affected warp mesh from (see surroundview.h).
class CalibrationCameraModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString identity READ identity CONSTANT)
    Q_PROPERTY(double posXMeters READ posXMeters WRITE setPosXMeters NOTIFY changed)
    Q_PROPERTY(double posYMeters READ posYMeters WRITE setPosYMeters NOTIFY changed)
    Q_PROPERTY(double posZMeters READ posZMeters WRITE setPosZMeters NOTIFY changed)
    Q_PROPERTY(double yawDegrees READ yawDegrees WRITE setYawDegrees NOTIFY changed)
    Q_PROPERTY(double pitchDegrees READ pitchDegrees WRITE setPitchDegrees NOTIFY changed)
    Q_PROPERTY(double fovDegrees READ fovDegrees WRITE setFovDegrees NOTIFY changed)

public:
    explicit CalibrationCameraModel(QObject *parent = nullptr) : QObject(parent) {}

    QString identity() const { return m_c.identity; }
    double posXMeters() const { return m_c.posXMeters; }
    double posYMeters() const { return m_c.posYMeters; }
    double posZMeters() const { return m_c.posZMeters; }
    double yawDegrees() const { return m_c.yawDegrees; }
    double pitchDegrees() const { return m_c.pitchDegrees; }
    double fovDegrees() const { return m_c.fovDegrees; }

    void setPosXMeters(double v) { if (v != m_c.posXMeters) { m_c.posXMeters = v; emit changed(); } }
    void setPosYMeters(double v) { if (v != m_c.posYMeters) { m_c.posYMeters = v; emit changed(); } }
    void setPosZMeters(double v) { if (v != m_c.posZMeters) { m_c.posZMeters = v; emit changed(); } }
    void setYawDegrees(double v) { if (v != m_c.yawDegrees) { m_c.yawDegrees = v; emit changed(); } }
    void setPitchDegrees(double v) { if (v != m_c.pitchDegrees) { m_c.pitchDegrees = v; emit changed(); } }
    void setFovDegrees(double v) { if (v != m_c.fovDegrees) { m_c.fovDegrees = v; emit changed(); } }

    // Bulk load/reset — sets every field from a struct with a single
    // changed() emission instead of one per field.
    void fromStruct(const CameraCalibration &c) { m_c = c; emit changed(); }

    // Fixed fields (image size, principal point, k1-k4) are carried through
    // from whatever this model was last loaded/reset from — not exposed as
    // Q_PROPERTYs, not user-tunable.
    CameraCalibration toStruct() const { return m_c; }

signals:
    void changed();

private:
    CameraCalibration m_c;
};

// QML-facing wrapper around SurroundGeometryConfig's user-tunable fields
// (vehicle footprint + ground capture extent + blend wedge). meshGridResolution
// is a render-quality knob, not a calibration value — stays code-only.
class CalibrationGeometryModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double vehicleLengthMeters READ vehicleLengthMeters WRITE setVehicleLengthMeters NOTIFY changed)
    Q_PROPERTY(double vehicleWidthMeters READ vehicleWidthMeters WRITE setVehicleWidthMeters NOTIFY changed)
    Q_PROPERTY(double groundHalfExtentXMeters READ groundHalfExtentXMeters WRITE setGroundHalfExtentXMeters NOTIFY changed)
    Q_PROPERTY(double groundHalfExtentYMeters READ groundHalfExtentYMeters WRITE setGroundHalfExtentYMeters NOTIFY changed)
    Q_PROPERTY(double wedgeOverlapDegrees READ wedgeOverlapDegrees WRITE setWedgeOverlapDegrees NOTIFY changed)

public:
    explicit CalibrationGeometryModel(QObject *parent = nullptr) : QObject(parent) {}

    double vehicleLengthMeters() const { return m_g.vehicleLengthMeters; }
    double vehicleWidthMeters() const { return m_g.vehicleWidthMeters; }
    double groundHalfExtentXMeters() const { return m_g.groundHalfExtentXMeters; }
    double groundHalfExtentYMeters() const { return m_g.groundHalfExtentYMeters; }
    double wedgeOverlapDegrees() const { return m_g.wedgeOverlapDegrees; }

    void setVehicleLengthMeters(double v) { if (v != m_g.vehicleLengthMeters) { m_g.vehicleLengthMeters = v; emit changed(); } }
    void setVehicleWidthMeters(double v) { if (v != m_g.vehicleWidthMeters) { m_g.vehicleWidthMeters = v; emit changed(); } }
    void setGroundHalfExtentXMeters(double v) { if (v != m_g.groundHalfExtentXMeters) { m_g.groundHalfExtentXMeters = v; emit changed(); } }
    void setGroundHalfExtentYMeters(double v) { if (v != m_g.groundHalfExtentYMeters) { m_g.groundHalfExtentYMeters = v; emit changed(); } }
    void setWedgeOverlapDegrees(double v) { if (v != m_g.wedgeOverlapDegrees) { m_g.wedgeOverlapDegrees = v; emit changed(); } }

    void fromStruct(const SurroundGeometryConfig &g) { m_g = g; emit changed(); }
    SurroundGeometryConfig toStruct() const { return m_g; }

signals:
    void changed();

private:
    SurroundGeometryConfig m_g;
};

// Persistent calibration state for the 360 view, backing
// CalibrationSettingsScreen.qml. Mirrors OdoStore's /data persistence
// pattern exactly (see odostore.h): same hardcoded absolute path on every
// platform, load-or-fall-back-to-defaults in the constructor, explicit
// save(). On macOS /data doesn't exist, so load() silently falls back to
// defaultCalibration() and save() fails-and-logs — same as OdoStore already
// does there, no special-casing needed.
class CalibrationStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CalibrationCameraModel *front READ front CONSTANT)
    Q_PROPERTY(CalibrationCameraModel *rear READ rear CONSTANT)
    Q_PROPERTY(CalibrationCameraModel *left READ left CONSTANT)
    Q_PROPERTY(CalibrationCameraModel *right READ right CONSTANT)
    Q_PROPERTY(CalibrationGeometryModel *geometry READ geometry CONSTANT)

public:
    explicit CalibrationStore(const QString &path, QObject *parent = nullptr);

    CalibrationCameraModel *front() const { return m_front; }
    CalibrationCameraModel *rear() const { return m_rear; }
    CalibrationCameraModel *left() const { return m_left; }
    CalibrationCameraModel *right() const { return m_right; }
    CalibrationGeometryModel *geometry() const { return m_geometry; }

    CalibrationSet toSet() const;

public slots:
    void save();
    void resetToDefaults();

signals:
    // Aggregated from all 5 sub-models' changed() signals — SurroundView
    // listens to this one signal rather than five.
    void calibrationChanged();

private:
    void applySet(const CalibrationSet &set); // no save(), used by load + reset
    void load();

    QString m_path;
    CalibrationCameraModel *m_front;
    CalibrationCameraModel *m_rear;
    CalibrationCameraModel *m_left;
    CalibrationCameraModel *m_right;
    CalibrationGeometryModel *m_geometry;
};

#endif
