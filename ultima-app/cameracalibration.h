#ifndef CAMERACALIBRATION_H
#define CAMERACALIBRATION_H

#include <QJsonObject>
#include <QString>

// One camera's calibration — intrinsics, fisheye model, and pose. WarpMesh
// is the only consumer of these fields; swapping defaultCalibration()'s
// placeholder numbers for real measured mount data later needs no renderer
// change, only new numbers.
//
// Fisheye model: equidistant (r = f * theta), the standard automotive/
// action-cam fisheye model. k1..k4 (Kannala-Brandt higher-order terms) are
// present but left at 0 (which reduces exactly to equidistant) for a future
// real-lens calibration to populate without a format change.
struct CameraCalibration {
    QString identity; // "front" | "rear" | "left" | "right"

    // Pose, vehicle-relative world coordinates in inches: X = forward
    // (nose), Y = left, Z = up. Matches WarpMesh's ground-plane convention.
    double posXInches = 0;
    double posYInches = 0;
    double posZInches = 0;
    double yawDegrees = 0;   // 0 = facing +X (forward), +90 = facing +Y (left)
    double pitchDegrees = 0; // 0 = horizontal, positive = tilted down

    int imageWidth = 1920;
    int imageHeight = 1080;
    double principalPointX = 0; // pixels; defaults to imageWidth/2 if 0
    double principalPointY = 0; // pixels; defaults to imageHeight/2 if 0
    double fovDegrees = 185.0;  // full FOV of the fisheye circle

    double k1 = 0, k2 = 0, k3 = 0, k4 = 0;

    double focalLengthPixels() const;
    double effectivePrincipalX() const { return principalPointX > 0 ? principalPointX : imageWidth / 2.0; }
    double effectivePrincipalY() const { return principalPointY > 0 ? principalPointY : imageHeight / 2.0; }

    // Same documented JSON shape test/avm-benchmark's CalibrationLoader
    // uses, so a real measured/auto-calibrated rig produced by either
    // project loads into the other unchanged.
    QJsonObject toJson() const;
    static CameraCalibration fromJson(const QJsonObject &o);
};

// Vehicle footprint, ground-capture extent, and blend-wedge parameters
// shared across all four cameras.
struct SurroundGeometryConfig {
    double vehicleLengthInches = 164.0; // real measured car: nose to tail
    double vehicleWidthInches = 75.0;   // real measured car: mirror to mirror
    double groundHalfExtentXInches = 216.5; // ground capture region: X in [-ext, ext]
    double groundHalfExtentYInches = 216.5; // ground capture region: Y in [-ext, ext]
    double wedgeOverlapDegrees = 20.0;    // angular feather width between adjacent cameras
    int meshGridResolution = 64;          // vertices per side of each camera's warp mesh

    // Screen-space scale for car_360.png's overlay in Camera360Screen.qml
    // (QML Image.scale, applied around the image's own center — which sits
    // at screen center same as the vehicle-mask hole it covers). Cosmetic
    // only: WarpMesh/SurroundView never read this, it doesn't touch any
    // real-world geometry above.
    double carIconScale = 1.0;

    QJsonObject toJson() const;
    static SurroundGeometryConfig fromJson(const QJsonObject &o);
};

struct CalibrationSet {
    CameraCalibration front, rear, left, right;
    SurroundGeometryConfig geometry;

    QJsonObject toJson() const;
    static CalibrationSet fromJson(const QJsonObject &o);
};

// PLACEHOLDER rig — nobody has measured this car's real camera mount
// positions/heights/yaw/pitch/FOV yet. Same generic sedan-ish numbers
// test/avm-benchmark's SyntheticCalibration used throughout its own
// development (front/rear cameras at bumper height, left/right at mirror
// height, all pitched down 52°, 185° fisheye). The stitch will run and
// blend correctly with these numbers, but seam alignment and ground-plane
// scale will only be right once real measured values replace them here —
// same "ask for real numbers" gap this project already has for Syvecs SCal
// data (see CLAUDE.md).
CalibrationSet defaultCalibration();

#endif
