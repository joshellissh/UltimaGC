#include "surroundview.h"
#include "warpmesh.h"
#include "shadermanager.h"
#include "surroundtexture.h"
#include "cameracalibration.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QDebug>
#include <cstdio>

namespace {

class SurroundViewRenderer : public QQuickFramebufferObject::Renderer {
public:
    ~SurroundViewRenderer() override {
        // Qt does not guarantee a current context at Renderer destruction —
        // same conservative guard test/avm-benchmark's renderer classes use.
        if (!m_initialized || !QOpenGLContext::currentContext()) return;
        auto *f = QOpenGLContext::currentContext()->functions();
        for (int i = 0; i < 4; ++i) {
            m_textures[i].destroy(f);
            m_meshes[i].destroy(f);
        }
    }

    // Called on the render thread with the GUI thread blocked — same
    // "safe without a lock" guarantee CameraFeed::currentFrame()'s own
    // comment relies on for CameraView, applied here instead.
    void synchronize(QQuickFramebufferObject *fboItem) override {
        auto *item = static_cast<SurroundView *>(fboItem);
        for (int i = 0; i < 4; ++i) {
            CameraFeed *feed = item->feedAt(i);
            if (!feed) continue;
            QImage frame = feed->currentFrame();
            if (!frame.isNull()) {
                m_pendingImage[i] = frame;
                m_pendingValid[i] = true;
            }
        }

        // Kept fresh every sync (cheap struct copy) so render()'s one-time
        // init block always has the latest calibration by the time it runs
        // — but only flagged dirty (triggering a mesh rebuild) on an actual
        // edit, via consumeCalibrationDirty().
        if (item->calibrationSource()) {
            m_pendingCalib = item->currentCalibration();
            if (item->consumeCalibrationDirty())
                m_pendingCalibDirty = true;
        }
    }

    // Rebuilds all 4 warp meshes (geometry only, not textures — textures
    // are image-sized and calibration-independent) from m_calib. Used both
    // for first-time init and for a live calibration edit; destroy() is
    // safe to call on a not-yet-uploaded WarpMesh (empty VAO/buffers).
    bool rebuildMeshes(QOpenGLFunctions *f) {
        const CameraCalibration *cams[4] = {&m_calib.front, &m_calib.rear, &m_calib.left, &m_calib.right};
        QSize viewportSize = framebufferObject()->size();
        for (int i = 0; i < 4; ++i) {
            m_meshes[i].destroy(f);
            if (!m_meshes[i].build(*cams[i], m_calib.geometry, BlendQuality::Feather, viewportSize) ||
                !m_meshes[i].upload(f)) {
                qWarning() << "SurroundView: failed to build warp mesh for" << cams[i]->identity;
                return false;
            }
        }
        return true;
    }

    void render() override {
        if (!m_initialized && !m_initFailed) {
            auto *f = QOpenGLContext::currentContext()->functions();
            m_calib = m_pendingCalib;
            m_prog = m_shaders.program(QStringLiteral(":/shaders/surround.vert"),
                                        QStringLiteral(":/shaders/surround.frag"));
            if (!m_prog) {
                qWarning() << "SurroundView: shader compile/link failed";
                m_initFailed = true;
                return;
            }
            if (!rebuildMeshes(f)) {
                m_initFailed = true;
                return;
            }
            m_initialized = true;
            fprintf(stderr, "[surroundview] initialized 4 cameras (front/rear/left/right)\n");
        }
        if (!m_initialized) return;

        // Texture allocation is deferred per-camera to that camera's first
        // real frame, sized from the frame itself rather than
        // cams[i]->imageWidth/imageHeight: CameraFeed may decode at less
        // than that calibration reference resolution (see camerafeed.cpp's
        // kDecimation — CameraGridScreen's 2026-08-17 perf fix, which this
        // screen shares CameraFeed with). Safe to do independently of the
        // calibration value: WarpMesh's texcoords are px/imageWidth,
        // py/imageHeight ratios (see warpmesh.cpp), i.e. normalized to
        // [0,1] and resolution-independent as long as the actual frame's
        // aspect ratio matches what calibration assumed — true for a
        // uniform decimation, which is all kDecimation does.
        auto *f = QOpenGLContext::currentContext()->functions();
        for (int i = 0; i < 4; ++i) {
            if (m_textureCreated[i] || !m_pendingValid[i]) continue;
            if (!m_textures[i].create(f, m_pendingImage[i].width(), m_pendingImage[i].height())) {
                qWarning() << "SurroundView: failed to allocate texture" << i;
                continue;
            }
            m_textureCreated[i] = true;
        }

        if (m_pendingCalibDirty) {
            m_calib = m_pendingCalib;
            m_pendingCalibDirty = false;
            rebuildMeshes(f); // on failure, keeps drawing the previous (still-valid) meshes
        }
        QSize sz = framebufferObject()->size();
        f->glViewport(0, 0, sz.width(), sz.height());
        f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        f->glClear(GL_COLOR_BUFFER_BIT);
        f->glEnable(GL_BLEND);
        f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        m_prog->bind();
        m_prog->setUniformValue("uBlendMode", 1); // use mesh weight as-is (feather)
        m_prog->setUniformValue("uTexture", 0);
        for (int i = 0; i < 4; ++i) {
            if (!m_textureCreated[i]) continue; // no frame yet to size this camera's texture from
            if (m_pendingValid[i]) {
                m_textures[i].upload(f, m_pendingImage[i]);
                m_pendingValid[i] = false;
            }
            m_textures[i].bind(f, 0);
            m_meshes[i].draw(f);
        }
        m_prog->release();
    }

    ShaderManager m_shaders;
    QOpenGLShaderProgram *m_prog = nullptr;
    CalibrationSet m_calib;
    WarpMesh m_meshes[4];
    SurroundTexture m_textures[4];
    QImage m_pendingImage[4];
    bool m_pendingValid[4] = {false, false, false, false};
    bool m_textureCreated[4] = {false, false, false, false};
    bool m_initialized = false;
    bool m_initFailed = false;
    CalibrationSet m_pendingCalib = defaultCalibration();
    bool m_pendingCalibDirty = false;
};

} // namespace

SurroundView::SurroundView(QQuickItem *parent) : QQuickFramebufferObject(parent) {}

SurroundView::~SurroundView() {
    // Feeds outlive this item (created in main() before the QML engine) —
    // leave their consumer counts balanced.
    if (m_consumersRegistered) {
        for (CameraFeed *feed : m_feeds)
            if (feed) feed->removeFrameConsumer();
        m_consumersRegistered = false;
    }
}

void SurroundView::updateFrameConsumers() {
    const bool want = isVisible();
    if (want == m_consumersRegistered)
        return;
    for (CameraFeed *feed : m_feeds) {
        if (!feed) continue;
        if (want) feed->addFrameConsumer();
        else feed->removeFrameConsumer();
    }
    m_consumersRegistered = want;
}

void SurroundView::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemVisibleHasChanged)
        updateFrameConsumers();
    QQuickFramebufferObject::itemChange(change, value);
}

QVariantList SurroundView::feeds() const {
    QVariantList list;
    for (CameraFeed *feed : m_feeds) list.append(QVariant::fromValue(static_cast<QObject *>(feed)));
    return list;
}

void SurroundView::setFeeds(const QVariantList &feeds) {
    // Deregister from the outgoing set first so counts stay balanced when
    // feeds are swapped while visible.
    const bool wasRegistered = m_consumersRegistered;
    if (wasRegistered) {
        for (CameraFeed *feed : m_feeds)
            if (feed) feed->removeFrameConsumer();
        m_consumersRegistered = false;
    }
    for (int i = 0; i < 4; ++i) {
        QObject::disconnect(m_frameConnections[i]);
        m_feeds[i] = (i < feeds.size()) ? qobject_cast<CameraFeed *>(feeds.at(i).value<QObject *>()) : nullptr;
        if (m_feeds[i]) {
            // Same isVisible() guard as CameraView::setFeed (see there):
            // QQuickFramebufferObject::update() re-renders the FBO even
            // for a hidden item, and this view is hidden most of the time.
            m_frameConnections[i] = connect(m_feeds[i], &CameraFeed::frameReady, this,
                                             [this]() { if (isVisible()) update(); });
        }
    }
    updateFrameConsumers();
    emit feedsChanged();
}

void SurroundView::setCalibrationSource(CalibrationStore *source) {
    if (source == m_calibrationSource) return;
    QObject::disconnect(m_calibrationConnection);
    m_calibrationSource = source;
    if (m_calibrationSource) {
        m_calibrationConnection = connect(m_calibrationSource, &CalibrationStore::calibrationChanged, this,
                                           [this]() { m_calibrationDirty = true; update(); });
    }
    m_calibrationDirty = true; // pick up the new source's values even before its first change
    update();
    emit calibrationSourceChanged();
}

QQuickFramebufferObject::Renderer *SurroundView::createRenderer() const {
    return new SurroundViewRenderer();
}
