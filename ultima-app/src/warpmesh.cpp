#include "warpmesh.h"

#include <QVector3D>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Geometry model — ported from test/avm-benchmark/src/graphics/WarpMesh.cpp.
//
// World/ground frame (vehicle-relative, inches): X = forward (nose), Y =
// left, Z = up. Each grid vertex starts life as a ground-plane point
// (X, Y, 0) inside the capture rectangle defined by
// SurroundGeometryConfig's ground_half_extent fields.
//
// Output NDC position: a straight top-down orthographic projection of that
// ground rectangle onto the render target, nose-up (screen "up" = vehicle
// +X, screen "right" = vehicle -Y since Y is left-positive), at a single
// uniform inches-per-pixel scale shared by both screen axes — so the
// capture rectangle's true shape is preserved instead of being stretched
// to fill the viewport. The capture rectangle need not be square and the
// viewport need not match its aspect ratio: whichever axis is more
// constrained (produces fewer pixels per inch) sets the shared scale, and
// the other axis then falls short of the viewport's edge — a letterboxed
// black bar — rather than being distorted to reach it. See the
// pixelsPerInch computation below.
//
// Camera projection (per vertex, per camera):
//   1. Build the camera's orthonormal (right, up, forward) basis in world
//      coordinates from its yaw (horizontal mount direction) and pitch
//      (downward tilt) — see cameraBasis() below.
//   2. Transform the ground point into camera space: X_c/Y_c/Z_c.
//   3. Reject points behind the camera (Z_c <= 0) or outside the fisheye's
//      half-FOV angle (equidistant model: theta = angle from the optical
//      axis, r = focalLengthPixels * theta) — these get weight 0 and are
//      simply invisible via alpha, not clipped from the mesh (cheaper than
//      dynamic topology, and blending naturally hides them).
//   4. Convert to pixel coordinates, then normalize to a [0,1] UV.
//
// Blend weight: each camera "owns" an azimuthal wedge of the ground
// (front/right/rear/left), extended by wedge_overlap_deg on each side and
// feathered to 0 across that overlap. The wedge boundaries are NOT a fixed
// 90 degrees apart — they're set so the seam between adjacent cameras runs
// straight out through the vehicle rectangle's actual corner, same as a
// real AVM system's stitch lines. For a non-square vehicle (this car is
// 164x75in) a fixed 45 degree half-wedge would put the front/rear seams
// well past the corner and the left/right seams well short of it, which is
// exactly what produced the black wedge-mismatch notches at the car icon's
// corners on real hardware — see cornerAngleDeg below.
// ---------------------------------------------------------------------------

namespace {

struct CameraBasis {
    QVector3D right, up, forward;
};

CameraBasis cameraBasis(double yawDeg, double pitchDeg) {
    double yaw = yawDeg * M_PI / 180.0;
    double pitch = pitchDeg * M_PI / 180.0;
    QVector3D yawDir(float(std::cos(yaw)), float(std::sin(yaw)), 0.0f);
    // Perpendicular horizontal "right" axis — invariant under the
    // subsequent pitch rotation since pitch rotates around this axis.
    QVector3D rightDir(yawDir.y(), -yawDir.x(), 0.0f);
    QVector3D worldUp(0.0f, 0.0f, 1.0f);

    CameraBasis b;
    b.right = rightDir;
    b.forward = (yawDir * float(std::cos(pitch)) - worldUp * float(std::sin(pitch))).normalized();
    b.up = QVector3D::crossProduct(b.right, b.forward).normalized();
    return b;
}

// Wedge center azimuth (degrees, world/ground frame) each camera owns.
double wedgeCenterDeg(const QString &identity) {
    if (identity == "front") return 0.0;
    if (identity == "right") return -90.0;
    if (identity == "rear") return 180.0;
    if (identity == "left") return 90.0;
    return 0.0;
}

double angularDistanceDeg(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
    return std::abs(d);
}

// Half-width (degrees) of this camera's full-weight wedge, given the
// vehicle rectangle's corner azimuth (measured from the front/rear axis —
// see cornerAngleDeg in build()). Front/rear own the slice from -corner to
// +corner around their axis (the ray from vehicle center through any point
// in that range crosses the vehicle's front/rear face); left/right own the
// remaining 90-corner on each side (crossing a side face instead). At
// corner=45deg (a square footprint) this reduces to the original fixed
// 45deg split.
double wedgeHalfWidthDeg(const QString &identity, double cornerAngleDeg) {
    if (identity == "front" || identity == "rear") return cornerAngleDeg;
    return 90.0 - cornerAngleDeg;
}

double wedgeWeight(double azimuthDeg, const QString &identity, double overlapDeg, double cornerAngleDeg,
                    BlendQuality quality) {
    double d = angularDistanceDeg(azimuthDeg, wedgeCenterDeg(identity));
    double half = wedgeHalfWidthDeg(identity, cornerAngleDeg);
    double fullHalf = half - overlapDeg / 2.0;
    double totalHalf = half + overlapDeg / 2.0;
    if (d <= fullHalf) return 1.0;
    if (d >= totalHalf) return 0.0;
    double t = (d - fullHalf) / (totalHalf - fullHalf);
    if (quality == BlendQuality::Improved) {
        // smoothstep falloff — the "improved seam" is a gentler S-curve,
        // not a different overlap region.
        double s = t * t * (3.0 - 2.0 * t);
        return 1.0 - s;
    }
    return 1.0 - t; // linear feather
}

} // namespace

bool WarpMesh::build(const CameraCalibration &cam, const SurroundGeometryConfig &geom, BlendQuality quality,
                      QSize viewportSize) {
    int res = std::max(2, geom.meshGridResolution);
    m_vertices.clear();
    m_indices.clear();
    m_vertices.reserve(size_t(res) * res);
    std::vector<bool> hasProjection; // true iff this vertex got a real projected UV, not the dummy
    hasProjection.reserve(size_t(res) * res);

    CameraBasis basis = cameraBasis(cam.yawDegrees, cam.pitchDegrees);
    QVector3D camPos(float(cam.posXInches), float(cam.posYInches), float(cam.posZInches));
    double thetaMax = (cam.fovDegrees / 2.0) * M_PI / 180.0;
    double f = cam.focalLengthPixels();
    double cx = cam.effectivePrincipalX();
    double cy = cam.effectivePrincipalY();
    double halfLen = geom.vehicleLengthInches / 2.0;
    double halfWid = geom.vehicleWidthInches / 2.0;

    // Azimuth (from the front axis) of the vehicle rectangle's corner —
    // see wedgeHalfWidthDeg()'s comment for why this, not a fixed 45deg,
    // sets the front/rear vs. left/right wedge split.
    double cornerAngleDeg = std::atan2(halfWid, halfLen) * 180.0 / M_PI;

    // Uniform inches-per-pixel scale shared by both screen axes — see this
    // file's header comment. viewportW/H fall back to 1 to avoid a
    // divide-by-zero if this ever runs before the FBO has a real size.
    double viewportW = std::max(1, viewportSize.width());
    double viewportH = std::max(1, viewportSize.height());
    double scaleFromDepth = (viewportH / 2.0) / geom.groundHalfExtentXInches;  // fwd/back maps to viewport height
    double scaleFromWidth = (viewportW / 2.0) / geom.groundHalfExtentYInches; // left/right maps to viewport width
    double pixelsPerInch = std::min(scaleFromDepth, scaleFromWidth);

    for (int gy = 0; gy < res; ++gy) {
        double ty = double(gy) / double(res - 1); // 0..1
        double gx0 = -geom.groundHalfExtentXInches + ty * 2.0 * geom.groundHalfExtentXInches;
        for (int gx = 0; gx < res; ++gx) {
            double tx = double(gx) / double(res - 1);
            double worldX = gx0; // ground X (forward) — varies with row (gy)
            double worldY = -geom.groundHalfExtentYInches + tx * 2.0 * geom.groundHalfExtentYInches;

            WarpVertex v{};
            // Nose-up birds-eye: screen Y follows vehicle +X, screen X
            // follows vehicle -Y (right is negative-Y since Y is left+).
            // Both use the same pixelsPerInch (see above) rather than each
            // independently normalizing to its own half-extent — the
            // latter always fills the full [-1,1] NDC range on both axes
            // regardless of the capture rectangle's real shape, which is
            // what silently produced the stretch this scale fixes.
            v.x = float(-worldY * pixelsPerInch / (viewportW / 2.0));
            v.y = float(worldX * pixelsPerInch / (viewportH / 2.0));

            bool insideVehicle = std::abs(worldX) < halfLen && std::abs(worldY) < halfWid;
            if (insideVehicle) {
                v.u = 0.5f; v.v = 0.5f; v.weight = 0.0f;
                m_vertices.push_back(v);
                hasProjection.push_back(false);
                continue;
            }

            QVector3D d = QVector3D(float(worldX), float(worldY), 0.0f) - camPos;
            double Xc = QVector3D::dotProduct(d, basis.right);
            double Yc = QVector3D::dotProduct(d, basis.up);
            double Zc = QVector3D::dotProduct(d, basis.forward);

            bool visible = true;
            double theta = 0, phi = 0;
            if (Zc <= 1e-4) {
                visible = false;
            } else {
                theta = std::atan2(std::sqrt(Xc * Xc + Yc * Yc), Zc);
                if (theta > thetaMax) visible = false;
                phi = std::atan2(Yc, Xc);
            }

            if (!visible) {
                v.u = 0.5f; v.v = 0.5f; v.weight = 0.0f;
                m_vertices.push_back(v);
                hasProjection.push_back(false);
                continue;
            }

            double r = f * theta;
            double px = cx + r * std::cos(phi);
            double py = cy + r * std::sin(phi);
            v.u = float(std::clamp(px / cam.imageWidth, 0.0, 1.0));
            v.v = float(std::clamp(py / cam.imageHeight, 0.0, 1.0));

            double azimuthDeg = std::atan2(worldY, worldX) * 180.0 / M_PI;
            v.weight = float(wedgeWeight(azimuthDeg, cam.identity, geom.wedgeOverlapDegrees, cornerAngleDeg, quality));

            m_vertices.push_back(v);
            hasProjection.push_back(true);
        }
    }

    for (int gy = 0; gy < res - 1; ++gy) {
        for (int gx = 0; gx < res - 1; ++gx) {
            uint32_t i0 = uint32_t(gy * res + gx);
            uint32_t i1 = uint32_t(gy * res + gx + 1);
            uint32_t i2 = uint32_t((gy + 1) * res + gx);
            uint32_t i3 = uint32_t((gy + 1) * res + gx + 1);

            // The fisheye projection's camera-local azimuth (phi =
            // atan2(Yc,Xc), used to place UVs) wraps at +-180 degrees. A
            // grid quad whose four corners straddle that wrap gets UVs
            // that are, numerically, on opposite sides of the texture even
            // though the ground points are adjacent; linearly interpolating
            // across the quad then samples a wide horizontal swath of the
            // source image instead of the small local patch it should.
            // Fix: skip emitting a quad whose corner UV.x values span more
            // than half the texture — a real discontinuity, not a
            // legitimately fast-changing but continuous warp.
            float minU = std::min({m_vertices[i0].u, m_vertices[i1].u, m_vertices[i2].u, m_vertices[i3].u});
            float maxU = std::max({m_vertices[i0].u, m_vertices[i1].u, m_vertices[i2].u, m_vertices[i3].u});
            if (maxU - minU > 0.5f) continue;

            // Second, related discontinuity: a vertex outside the camera's
            // FOV or inside the vehicle mask has no real projected UV, so
            // it gets a dummy uv=(0.5,0.5) — see hasProjection[] below. A
            // quad with some corners real and one dummy interpolates from a
            // real UV toward that arbitrary value while nonzero weight at
            // the other corners still pulls fragment alpha above 0, so the
            // wrong sampled color shows through instead of being safely
            // masked. Checking hasProjection rather than weight==0 is what
            // keeps this from also skipping the legitimate feather gradient.
            if (hasProjection[i0] != hasProjection[i1] || hasProjection[i0] != hasProjection[i2] ||
                hasProjection[i0] != hasProjection[i3]) {
                continue;
            }

            m_indices.insert(m_indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    return !m_vertices.empty();
}

bool WarpMesh::upload(QOpenGLFunctions *f) {
    if (m_vertices.empty()) return false;

    m_vao = std::make_unique<QOpenGLVertexArrayObject>();
    if (!m_vao->create()) return false;
    m_vao->bind();

    if (!m_vbo.create()) { m_vao->release(); return false; }
    m_vbo.bind();
    m_vbo.allocate(m_vertices.data(), int(m_vertices.size() * sizeof(WarpVertex)));

    if (!m_ibo.create()) { m_vao->release(); return false; }
    m_ibo.bind();
    m_ibo.allocate(m_indices.data(), int(m_indices.size() * sizeof(uint32_t)));

    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(WarpVertex),
                              reinterpret_cast<void *>(offsetof(WarpVertex, x)));
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WarpVertex),
                              reinterpret_cast<void *>(offsetof(WarpVertex, u)));
    f->glEnableVertexAttribArray(2);
    f->glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(WarpVertex),
                              reinterpret_cast<void *>(offsetof(WarpVertex, weight)));

    m_vao->release();
    m_vbo.release();
    m_ibo.release();
    return true;
}

void WarpMesh::draw(QOpenGLFunctions *f) {
    if (!m_vao) return;
    m_vao->bind();
    f->glDrawElements(GL_TRIANGLES, GLsizei(m_indices.size()), GL_UNSIGNED_INT, nullptr);
    m_vao->release();
}

void WarpMesh::destroy(QOpenGLFunctions *) {
    // QOpenGLBuffer::destroy() needs no functions pointer (it goes through
    // its own internal QOpenGLContext-current lookup) — the parameter
    // exists to match this project's other GL resource classes' signatures.
    if (m_vbo.isCreated()) m_vbo.destroy();
    if (m_ibo.isCreated()) m_ibo.destroy();
    m_vao.reset(); // QOpenGLVertexArrayObject's destructor releases it.
}
