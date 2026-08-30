#include "cameraview.h"
#include "shadermanager.h"
#include "surroundtexture.h"
#include "dmabuftexture.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QVector3D>
#include <QDebug>
#include <QElapsedTimer>
#include <cmath>
#include <algorithm>
#include <stdio.h>

namespace {

// ---------------------------------------------------------------------------
// Mirror-view reprojection (leftCamOverlay/rightCamOverlay only — see
// mirror.frag). PLACEHOLDER numbers throughout: found empirically against a
// hand-sketched target-FOV diagram (apex at the mirror, fanning rearward
// along the flank), not a real measured mount — same "ask for real numbers"
// gap cameracalibration.h's defaultCalibration() already documents for the
// ground-plane stitch. Ported from a standalone ffmpeg (v360 filter)
// prototype; kMirrorInputFovDeg=180 matches that prototype's ih_fov/iv_fov
// exactly rather than reusing cameracalibration.h's own (also-placeholder,
// also-unverified) 185deg figure — matching the already-visually-verified
// prototype mattered more here than reusing an unrelated placeholder.
constexpr double kMirrorInputFovDeg = 180.0;

struct MirrorViewParams {
    double yawDeg = 0, pitchDeg = 0, rollDeg = 0;
    double hFovDeg = 65.0, vFovDeg = 50.0;
};

MirrorViewParams mirrorViewParams(const QString &side) {
    // roll's sign here is the OPPOSITE of what the standalone ffmpeg v360
    // prototype needed (-60/+60) for the same visual result — ffmpeg's
    // internal axis convention for its rotation order isn't documented, and
    // evidently doesn't match rotatedBasis()'s below. Found by re-tuning
    // visually against the live macOS render rather than chasing ffmpeg
    // parity: yaw/pitch (which the center-ray direction alone validates,
    // roll-independent) carried over fine, only roll's sign needed to flip,
    // and its magnitude came down slightly (60->50) once the sign was right.
    MirrorViewParams p;
    if (side == QLatin1String("left")) {
        p.yawDeg = -60.0;
        p.pitchDeg = 10.0;
        p.rollDeg = 50.0;
    } else { // "right"
        p.yawDeg = 60.0;
        p.pitchDeg = 10.0;
        p.rollDeg = -50.0;
    }
    return p;
}

struct AxisBasis { QVector3D right, up, forward; };

// Camera-local basis (right=+X, up=+Y, forward=+Z/optical axis), rotated by
// yaw (about up) then pitch (about the resulting right) then roll (about the
// resulting forward) — same intrinsic "yaw, pitch, roll" composition order
// ffmpeg's v360 filter uses (rorder="ypr", its default — the order these
// numbers were tuned against), just camera-local instead of warpmesh.cpp's
// cameraBasis() (vehicle/world-relative, no roll term, only used for the
// separate ground-plane stitch). Returns the rotated axes expressed in the
// ORIGINAL (physical-camera) frame — exactly what a ray defined in the
// rotated (virtual-camera) frame needs to be linearly combined against to
// land in physical-camera coordinates.
AxisBasis rotatedBasis(double yawDeg, double pitchDeg, double rollDeg) {
    double yaw = yawDeg * M_PI / 180.0;
    double pitch = pitchDeg * M_PI / 180.0;
    double roll = rollDeg * M_PI / 180.0;

    QVector3D right(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f), forward(0.0f, 0.0f, 1.0f);

    QVector3D right1 = right * float(std::cos(yaw)) - forward * float(std::sin(yaw));
    QVector3D forward1 = right * float(std::sin(yaw)) + forward * float(std::cos(yaw));
    QVector3D up1 = up;

    QVector3D up2 = up1 * float(std::cos(pitch)) - forward1 * float(std::sin(pitch));
    QVector3D forward2 = up1 * float(std::sin(pitch)) + forward1 * float(std::cos(pitch));
    QVector3D right2 = right1;

    QVector3D right3 = right2 * float(std::cos(roll)) - up2 * float(std::sin(roll));
    QVector3D up3 = right2 * float(std::sin(roll)) + up2 * float(std::cos(roll));
    QVector3D forward3 = forward2;

    return {right3, up3, forward3};
}

// NDC quad covering the whole FBO, paired with blit.vert/blit.frag — no
// warp mesh, this is CameraGridScreen's plain un-stitched view. v=1 at the
// QImage's first (top) row: SurroundTexture::upload() writes QImage
// scanlines in memory order with no row reversal, and OpenGL's texture (s,t)
// convention has t=0 at the *bottom* of the image, so the image's first
// (top) row lands at the texture's t=0 end — sampling needs v=1 at NDC's
// top edge to come out right-side up. Confirmed empirically on the macOS
// dev build (2026-08-17): the naive v=0-at-top mapping rendered the
// simulated feed's overlay text upside down.
constexpr GLfloat kQuadVertices[] = {
    // x,     y,    u,    v
    -1.0f,  1.0f, 0.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
};

class CameraViewRenderer : public QQuickFramebufferObject::Renderer {
public:
    ~CameraViewRenderer() override {
        // Same conservative guard SurroundViewRenderer uses — Qt does not
        // guarantee a current context at Renderer destruction. m_dmabuf's
        // own destructor still releases its borrowed capture buffers even
        // when the GL objects can't be freed here.
        if (!m_initialized || !QOpenGLContext::currentContext()) return;
        auto *f = QOpenGLContext::currentContext()->functions();
        m_dmabuf.destroy(f);
        m_texture.destroy(f);
        m_vbo.destroy();
        m_vao.destroy();
    }

    // Called on the render thread with the GUI thread blocked — same
    // "safe without a lock" guarantee CameraFeed::currentFrame()'s own
    // comment relies on, mirrors SurroundViewRenderer::synchronize().
    void synchronize(QQuickFramebufferObject *fboItem) override {
        auto *item = static_cast<CameraView *>(fboItem);
        m_mirrorSide = item->mirrorViewSide();
        CameraFeed *feed = item->feed();
        if (!feed) return;
        // Zero-copy display path (see dmabuftexture.h): borrow the newest
        // capture buffer instead of copying a converted QImage out. The GL
        // context is current during sync — DmaBufTextureSet relies on that
        // to drop stale imports the moment a stream restarts.
        if (feed->zeroCopy() && DmaBufTextureSet::available()) {
            QOpenGLContext *ctx = QOpenGLContext::currentContext();
            m_dmabuf.sync(feed, ctx ? ctx->functions() : nullptr);
            return;
        }
        QImage frame = feed->currentFrame();
        if (!frame.isNull()) {
            m_pendingImage = frame;
            m_pendingValid = true;
        }
    }

    void render() override {
        auto *f = QOpenGLContext::currentContext()->functions();

        if (!m_initialized && !m_initFailed) {
            m_prog = m_shaders.program(QStringLiteral(":/shaders/blit.vert"),
                                        QStringLiteral(":/shaders/blit.frag"));
            if (!m_prog) {
                qWarning() << "CameraView: shader compile/link failed";
                m_initFailed = true;
                return;
            }
            // Same blit.vert, different fragment shader — see mirror.frag's
            // header comment. Not fatal if this one fails: falls back to
            // the raw blit program below rather than blanking the whole
            // view, since a typo here shouldn't take out CameraGridScreen
            // too (it never sets mirrorViewSide, so never reaches this
            // program regardless).
            m_progMirror = m_shaders.program(QStringLiteral(":/shaders/blit.vert"),
                                              QStringLiteral(":/shaders/mirror.frag"));
            if (!m_progMirror)
                qWarning() << "CameraView: mirror shader compile/link failed, falling back to raw";
            // Desktop GL 3.3 core (see main.cpp's QSurfaceFormat comment)
            // rejects glDrawArrays with no VAO bound — GLES 3.x's default
            // VAO 0 is usable so the target wouldn't have caught this, but a
            // real VBO+VAO is the portable, correct way to feed vertex
            // attributes either way, not a desktop-only workaround.
            m_vao.create();
            m_vao.bind();
            m_vbo.create();
            m_vbo.bind();
            m_vbo.allocate(kQuadVertices, sizeof(kQuadVertices));
            m_prog->enableAttributeArray(0);
            m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
            m_prog->enableAttributeArray(1);
            m_prog->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
            m_vao.release();
            m_vbo.release();
            m_initialized = true;
        }
        if (!m_initialized) return;

        if (m_dmabuf.hasFrame()) {
            renderZeroCopy(f);
            return;
        }

        // Texture allocation is deferred to the first real frame: CameraFeed
        // doesn't know its negotiated size until then (frameWidth/frameHeight
        // start at 0x0 — see camerafeed.h), unlike SurroundView's textures,
        // which size from fixed calibration data available up front.
        if (!m_textureCreated) {
            if (!m_pendingValid) return; // nothing to size the texture from yet
            if (!m_texture.create(f, m_pendingImage.width(), m_pendingImage.height())) {
                qWarning() << "CameraView: failed to allocate texture";
                return;
            }
            m_textureCreated = true;
        }

        QSize sz = framebufferObject()->size();
        f->glViewport(0, 0, sz.width(), sz.height());
        f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        f->glClear(GL_COLOR_BUFFER_BIT);

        // Perf instrumentation, opt-in via ULTIMA_CAM_FPS_LOG (same flag as
        // camerafeed.cpp's) — separating "how often does this view render"
        // from "what does the texture upload cost" was what localized the
        // 2026-08-26 render-thread bottleneck; cheap enough to keep.
        static const bool perfLog = qEnvironmentVariableIsSet("ULTIMA_CAM_FPS_LOG");
        QElapsedTimer t;
        if (perfLog) t.start();
        if (m_pendingValid) {
            m_texture.upload(f, m_pendingImage);
            m_pendingValid = false;
            if (perfLog) { m_uploadNsSum += t.nsecsElapsed(); ++m_uploadCount; }
        }

        bool useMirror = m_progMirror && (m_mirrorSide == QLatin1String("left") || m_mirrorSide == QLatin1String("right"));
        QOpenGLShaderProgram *activeProg = useMirror ? m_progMirror : m_prog;

        m_vao.bind();
        activeProg->bind();
        activeProg->setUniformValue("uTexture", 0);
        if (useMirror) {
            MirrorViewParams p = mirrorViewParams(m_mirrorSide);
            AxisBasis basis = rotatedBasis(p.yawDeg, p.pitchDeg, p.rollDeg);
            double thetaMax = (kMirrorInputFovDeg / 2.0) * M_PI / 180.0;
            // Width, not min(width,height) -- empirically calibrated against
            // the ffmpeg v360 prototype these numbers came from (near-zero
            // color error across a full horizontal profile at this value;
            // min(w,h) was off by a consistent ~1.78x). ffmpeg's own
            // "fisheye" input convention isn't documented; this was fit by
            // comparison against known output, not derived from source.
            double focalPx = (m_pendingImage.width() / 2.0) / thetaMax;
            activeProg->setUniformValue("uRight", basis.right);
            activeProg->setUniformValue("uUp", basis.up);
            activeProg->setUniformValue("uForward", basis.forward);
            activeProg->setUniformValue("uTanHalfHFov", GLfloat(std::tan(p.hFovDeg / 2.0 * M_PI / 180.0)));
            activeProg->setUniformValue("uTanHalfVFov", GLfloat(std::tan(p.vFovDeg / 2.0 * M_PI / 180.0)));
            activeProg->setUniformValue("uFocalPx", GLfloat(focalPx));
            activeProg->setUniformValue("uThetaMaxRad", GLfloat(thetaMax));
            activeProg->setUniformValue("uPrincipalXFrac", GLfloat(0.5));
            activeProg->setUniformValue("uPrincipalYFrac", GLfloat(0.5));
        }
        m_texture.bind(f, 0);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        activeProg->release();
        m_vao.release();
        if (perfLog) logRenderStats("upload", t.nsecsElapsed());
    }

    // Zero-copy draw: same quad and (optionally) same mirror reprojection
    // as the QImage path, but sampling the camera's dma-buf directly
    // through a samplerExternalOES program (ShaderManager::ExternalSampler
    // rewrites the identical .frag sources — no duplicate shader files).
    void renderZeroCopy(QOpenGLFunctions *f) {
        if (!m_progExt) {
            m_progExt = m_shaders.program(QStringLiteral(":/shaders/blit.vert"),
                                          QStringLiteral(":/shaders/blit.frag"),
                                          ShaderManager::ExternalSampler);
            m_progExtMirror = m_shaders.program(QStringLiteral(":/shaders/blit.vert"),
                                                QStringLiteral(":/shaders/mirror.frag"),
                                                ShaderManager::ExternalSampler);
            if (!m_progExt) {
                qWarning() << "CameraView: external-sampler shader failed — zero-copy frames won't display";
                return;
            }
        }
        static const bool perfLog = qEnvironmentVariableIsSet("ULTIMA_CAM_FPS_LOG");
        QElapsedTimer t;
        if (perfLog) t.start();

        QSize sz = framebufferObject()->size();
        if (perfLog && !m_zcAnnounced) {
            // One line per renderer so the fps stats below are attributable
            // to a concrete view (grid quadrant / rear / mirror overlay).
            fprintf(stderr, "[cameraview %p] zero-copy start: fbo %dx%d mirror='%s'\n",
                    static_cast<void *>(this), sz.width(), sz.height(), qPrintable(m_mirrorSide));
            m_zcAnnounced = true;
        }
        f->glViewport(0, 0, sz.width(), sz.height());
        f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        f->glClear(GL_COLOR_BUFFER_BIT);

        bool useMirror = m_progExtMirror && (m_mirrorSide == QLatin1String("left") || m_mirrorSide == QLatin1String("right"));
        QOpenGLShaderProgram *activeProg = useMirror ? m_progExtMirror : m_progExt;

        m_vao.bind();
        activeProg->bind();
        activeProg->setUniformValue("uTexture", 0);
        if (useMirror) {
            MirrorViewParams p = mirrorViewParams(m_mirrorSide);
            AxisBasis basis = rotatedBasis(p.yawDeg, p.pitchDeg, p.rollDeg);
            double thetaMax = (kMirrorInputFovDeg / 2.0) * M_PI / 180.0;
            // Width in this path is the full capture width (the dma-buf is
            // the raw 1920-wide frame, not the decimated QImage) — the
            // shader divides by textureSize() of the same texture, so the
            // two stay consistent. See the QImage path's focalPx comment
            // for where the formula itself came from.
            double focalPx = (m_dmabuf.width() / 2.0) / thetaMax;
            activeProg->setUniformValue("uRight", basis.right);
            activeProg->setUniformValue("uUp", basis.up);
            activeProg->setUniformValue("uForward", basis.forward);
            activeProg->setUniformValue("uTanHalfHFov", GLfloat(std::tan(p.hFovDeg / 2.0 * M_PI / 180.0)));
            activeProg->setUniformValue("uTanHalfVFov", GLfloat(std::tan(p.vFovDeg / 2.0 * M_PI / 180.0)));
            activeProg->setUniformValue("uFocalPx", GLfloat(focalPx));
            activeProg->setUniformValue("uThetaMaxRad", GLfloat(thetaMax));
            activeProg->setUniformValue("uPrincipalXFrac", GLfloat(0.5));
            activeProg->setUniformValue("uPrincipalYFrac", GLfloat(0.5));
        }
        if (m_dmabuf.bind(f, 0))
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        activeProg->release();
        m_vao.release();
        if (perfLog) logRenderStats("zero-copy", t.nsecsElapsed());
    }

    void logRenderStats(const char *tag, qint64 renderNs) {
        m_renderNsSum += renderNs;
        ++m_renderCount;
        if (!m_perfTimer.isValid()) m_perfTimer.start();
        if (m_perfTimer.elapsed() < 2000) return;
        if (m_uploadCount > 0)
            fprintf(stderr, "[cameraview %p] %s: %d renders (%.1f/s), render() %.2f ms avg, upload %.2f ms avg over %d\n",
                    static_cast<void *>(this), tag, m_renderCount,
                    m_renderCount * 1000.0 / m_perfTimer.elapsed(),
                    m_renderNsSum / double(m_renderCount) / 1e6,
                    m_uploadNsSum / double(m_uploadCount) / 1e6, m_uploadCount);
        else
            fprintf(stderr, "[cameraview %p] %s: %d renders (%.1f/s), render() %.2f ms avg\n",
                    static_cast<void *>(this), tag, m_renderCount,
                    m_renderCount * 1000.0 / m_perfTimer.elapsed(),
                    m_renderNsSum / double(m_renderCount) / 1e6);
        m_renderNsSum = m_uploadNsSum = 0;
        m_renderCount = m_uploadCount = 0;
        m_perfTimer.restart();
    }

    DmaBufTextureSet m_dmabuf;
    bool m_zcAnnounced = false;
    QOpenGLShaderProgram *m_progExt = nullptr;
    QOpenGLShaderProgram *m_progExtMirror = nullptr;
    QElapsedTimer m_perfTimer;
    qint64 m_uploadNsSum = 0, m_renderNsSum = 0;
    int m_uploadCount = 0, m_renderCount = 0;

    ShaderManager m_shaders;
    QOpenGLShaderProgram *m_prog = nullptr;
    QOpenGLShaderProgram *m_progMirror = nullptr;
    QString m_mirrorSide;
    SurroundTexture m_texture;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QImage m_pendingImage;
    bool m_pendingValid = false;
    bool m_textureCreated = false;
    bool m_initialized = false;
    bool m_initFailed = false;
};

} // namespace

CameraView::CameraView(QQuickItem *parent) : QQuickFramebufferObject(parent) {}

void CameraView::setFeed(CameraFeed *feed)
{
    if (feed == m_feed)
        return;

    QObject::disconnect(m_frameConnection);
    QObject::disconnect(m_streamConnection);
    m_feed = feed;
    if (m_feed) {
        // isVisible() guard on both: QQuickFramebufferObject::update()
        // schedules the FBO render pass directly on the scene-graph node,
        // BYPASSING item visibility — without the guard every CameraView
        // bound to a streaming feed keeps re-rendering its FBO while
        // hidden. Measured on hardware (2026-08-26): the two hidden mirror
        // overlays' per-fragment reprojection alone dragged the whole
        // window from 25fps to 17fps in the 4-camera fanout bench. A view
        // that becomes visible again repaints via the visibility change
        // itself, then follows frames normally from the next frameReady.
        m_frameConnection = connect(m_feed, &CameraFeed::frameReady, this, [this]() {
            if (isVisible())
                update();
        });
        // See cameraview.h's m_streamConnection comment — one extra sync
        // after a stream ends lets the renderer drop its dma-buf imports.
        m_streamConnection = connect(m_feed, &CameraFeed::streamingChanged, this, [this]() {
            if (isVisible())
                update();
        });
    }
    emit feedChanged();
    update();
}

void CameraView::setMirrorViewSide(const QString &side)
{
    if (side == m_mirrorViewSide)
        return;
    m_mirrorViewSide = side;
    emit mirrorViewSideChanged();
    update();
}

QQuickFramebufferObject::Renderer *CameraView::createRenderer() const
{
    return new CameraViewRenderer();
}

QSGNode *CameraView::updatePaintNode(QSGNode *node, UpdatePaintNodeData *data)
{
    // Qt Quick calls updatePaintNode() on a brand-new QQuickFramebufferObject
    // even when it (or an ancestor) is hidden, and the base implementation
    // then creates the Renderer and the FBO right away and hooks the FBO
    // render pass onto QQuickWindow::beforeRendering — so at boot all five
    // CameraViews (four in CameraGridScreen, one in RearCameraScreen, none
    // of them on screen) each allocated an FBO and ran render() once, i.e.
    // ten PowerVR GLSL compiles inside the very first frame. Measured on the
    // BeagleY-AI (2026-08-30, QSG_RENDER_TIMING): the first frame's render
    // phase was ~485 ms of which the scene-graph renderer itself was 105.
    // While hidden and node-less, return nothing; a visibility change marks
    // the item dirty (QQuickItemPrivate::Visible is in ContentUpdateMask),
    // so the node gets built the first time the view actually shows. An
    // existing node is kept across hide/show as before.
    if (!node && !isVisible())
        return nullptr;
    return QQuickFramebufferObject::updatePaintNode(node, data);
}
