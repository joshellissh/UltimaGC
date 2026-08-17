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

namespace {
CameraCalibration makeCamera(const QString &identity, double x, double y, double z,
                              double yawDeg, double pitchDeg, int width, int height, double fovDeg) {
    CameraCalibration c;
    c.identity = identity;
    c.posXMeters = x;
    c.posYMeters = y;
    c.posZMeters = z;
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

    // Typical compact-sedan-ish dimensions; only used to place cameras and
    // mask the vehicle footprint, not rendered as a real model. See this
    // file's header comment — replace with real measured values.
    const double halfLength = 2.25;  // bumper-to-bumper / 2
    const double halfWidth = 0.95;   // mirror-to-mirror-ish / 2
    const double mountHeightFront = 0.75;
    const double mountHeightSide = 0.95; // side mirrors sit higher than bumpers
    const double pitchDeg = 52.0;        // downward tilt, all four cameras

    set.front = makeCamera("front", halfLength, 0.0, mountHeightFront, 0.0, pitchDeg, width, height, fovDeg);
    set.rear = makeCamera("rear", -halfLength, 0.0, mountHeightFront, 180.0, pitchDeg, width, height, fovDeg);
    set.left = makeCamera("left", 0.0, halfWidth, mountHeightSide, 90.0, pitchDeg, width, height, fovDeg);
    set.right = makeCamera("right", 0.0, -halfWidth, mountHeightSide, -90.0, pitchDeg, width, height, fovDeg);

    set.geometry.vehicleLengthMeters = halfLength * 2.0;
    set.geometry.vehicleWidthMeters = halfWidth * 2.0;
    set.geometry.groundHalfExtentXMeters = 5.5;
    set.geometry.groundHalfExtentYMeters = 5.5;
    set.geometry.wedgeOverlapDegrees = 20.0;
    set.geometry.meshGridResolution = 64;

    return set;
}
