#include "cameracalibration.h"

#include <cmath>
#include <algorithm>

double CameraCalibration::focalLengthPixels() const {
    // Equidistant fisheye, sized so the FOV circle exactly inscribes the
    // shorter image dimension (the common automotive fisheye crop).
    double thetaMax = (fovDegrees / 2.0) * M_PI / 180.0;
    double rMax = std::min(imageWidth, imageHeight) / 2.0;
    return thetaMax > 0 ? rMax / thetaMax : 1.0;
}

// Same documented shape as test/avm-benchmark's CalibrationLoader — a real
// measured or auto-calibrated rig produced by either project loads into
// the other unchanged.
QJsonObject CameraCalibration::toJson() const {
    QJsonObject o;
    o["identity"] = identity;
    o["pos_x_in"] = posXInches;
    o["pos_y_in"] = posYInches;
    o["pos_z_in"] = posZInches;
    o["yaw_deg"] = yawDegrees;
    o["pitch_deg"] = pitchDegrees;
    o["image_width"] = imageWidth;
    o["image_height"] = imageHeight;
    o["principal_point_x"] = principalPointX;
    o["principal_point_y"] = principalPointY;
    o["fov_deg"] = fovDegrees;
    o["k1"] = k1;
    o["k2"] = k2;
    o["k3"] = k3;
    o["k4"] = k4;
    return o;
}

CameraCalibration CameraCalibration::fromJson(const QJsonObject &o) {
    CameraCalibration c;
    c.identity = o["identity"].toString(c.identity);
    c.posXInches = o["pos_x_in"].toDouble(c.posXInches);
    c.posYInches = o["pos_y_in"].toDouble(c.posYInches);
    c.posZInches = o["pos_z_in"].toDouble(c.posZInches);
    c.yawDegrees = o["yaw_deg"].toDouble(c.yawDegrees);
    c.pitchDegrees = o["pitch_deg"].toDouble(c.pitchDegrees);
    c.imageWidth = o["image_width"].toInt(c.imageWidth);
    c.imageHeight = o["image_height"].toInt(c.imageHeight);
    c.principalPointX = o["principal_point_x"].toDouble(c.principalPointX);
    c.principalPointY = o["principal_point_y"].toDouble(c.principalPointY);
    c.fovDegrees = o["fov_deg"].toDouble(c.fovDegrees);
    c.k1 = o["k1"].toDouble(c.k1);
    c.k2 = o["k2"].toDouble(c.k2);
    c.k3 = o["k3"].toDouble(c.k3);
    c.k4 = o["k4"].toDouble(c.k4);
    return c;
}

QJsonObject SurroundGeometryConfig::toJson() const {
    QJsonObject o;
    o["vehicle_length_in"] = vehicleLengthInches;
    o["vehicle_width_in"] = vehicleWidthInches;
    o["ground_half_extent_x_in"] = groundHalfExtentXInches;
    o["ground_half_extent_y_in"] = groundHalfExtentYInches;
    o["wedge_overlap_deg"] = wedgeOverlapDegrees;
    o["mesh_grid_resolution"] = meshGridResolution;
    o["car_icon_scale"] = carIconScale;
    return o;
}

SurroundGeometryConfig SurroundGeometryConfig::fromJson(const QJsonObject &o) {
    SurroundGeometryConfig g;
    g.vehicleLengthInches = o["vehicle_length_in"].toDouble(g.vehicleLengthInches);
    g.vehicleWidthInches = o["vehicle_width_in"].toDouble(g.vehicleWidthInches);
    g.groundHalfExtentXInches = o["ground_half_extent_x_in"].toDouble(g.groundHalfExtentXInches);
    g.groundHalfExtentYInches = o["ground_half_extent_y_in"].toDouble(g.groundHalfExtentYInches);
    g.wedgeOverlapDegrees = o["wedge_overlap_deg"].toDouble(g.wedgeOverlapDegrees);
    g.meshGridResolution = o["mesh_grid_resolution"].toInt(g.meshGridResolution);
    g.carIconScale = o["car_icon_scale"].toDouble(g.carIconScale);
    return g;
}

QJsonObject CalibrationSet::toJson() const {
    QJsonObject o;
    o["format"] = "ultima_calibration_v1";
    o["front"] = front.toJson();
    o["rear"] = rear.toJson();
    o["left"] = left.toJson();
    o["right"] = right.toJson();
    o["geometry"] = geometry.toJson();
    return o;
}

CalibrationSet CalibrationSet::fromJson(const QJsonObject &o) {
    CalibrationSet s;
    s.front = CameraCalibration::fromJson(o["front"].toObject());
    s.rear = CameraCalibration::fromJson(o["rear"].toObject());
    s.left = CameraCalibration::fromJson(o["left"].toObject());
    s.right = CameraCalibration::fromJson(o["right"].toObject());
    s.geometry = SurroundGeometryConfig::fromJson(o["geometry"].toObject());
    return s;
}

namespace {
CameraCalibration makeCamera(const QString &identity, double x, double y, double z,
                              double yawDeg, double pitchDeg, int width, int height, double fovDeg) {
    CameraCalibration c;
    c.identity = identity;
    c.posXInches = x;
    c.posYInches = y;
    c.posZInches = z;
    c.yawDegrees = yawDeg;
    c.pitchDegrees = pitchDeg;
    c.imageWidth = width;
    c.imageHeight = height;
    c.fovDegrees = fovDeg;
    return c;
}
} // namespace

CalibrationSet defaultCalibration() {
    CalibrationSet set;

    const int width = 1920, height = 1080;
    const double fovDeg = 185.0;

    // Real measured car: 164in nose-to-tail, 75in mirror-to-mirror — also
    // used to place the front/rear/left/right cameras at the vehicle's
    // extremities. Mount height/pitch are still placeholders (not measured
    // yet) — see this file's header comment.
    const double halfLength = 82.0;  // 164in / 2, bumper-to-bumper
    const double halfWidth = 37.5;   // 75in / 2, mirror-to-mirror
    const double mountHeightFront = 29.5;
    const double mountHeightSide = 37.4; // side mirrors sit higher than bumpers
    const double pitchDeg = 52.0;        // downward tilt, all four cameras

    set.front = makeCamera("front", halfLength, 0.0, mountHeightFront, 0.0, pitchDeg, width, height, fovDeg);
    set.rear = makeCamera("rear", -halfLength, 0.0, mountHeightFront, 180.0, pitchDeg, width, height, fovDeg);
    set.left = makeCamera("left", 0.0, halfWidth, mountHeightSide, 90.0, pitchDeg, width, height, fovDeg);
    set.right = makeCamera("right", 0.0, -halfWidth, mountHeightSide, -90.0, pitchDeg, width, height, fovDeg);

    set.geometry.vehicleLengthInches = halfLength * 2.0;
    set.geometry.vehicleWidthInches = halfWidth * 2.0;
    set.geometry.groundHalfExtentXInches = 216.5;
    set.geometry.groundHalfExtentYInches = 216.5;
    set.geometry.wedgeOverlapDegrees = 20.0;
    set.geometry.meshGridResolution = 64;

    return set;
}
