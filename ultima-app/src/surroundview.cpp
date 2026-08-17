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
    }

    void render() override {
        if (!m_initialized && !m_initFailed) {
            auto *f = QOpenGLContext::currentContext()->functions();
            m_calib = defaultCalibration();
            m_prog = m_shaders.program(QStringLiteral(":/shaders/surround.vert"),
                                        QStringLiteral(":/shaders/surround.frag"));
            if (!m_prog) {
                qWarning() << "SurroundView: shader compile/link failed";
                m_initFailed = true;
                return;
            }
            const CameraCalibration *cams[4] = {&m_calib.front, &m_calib.rear, &m_calib.left, &m_calib.right};
            for (int i = 0; i < 4; ++i) {
                if (!m_meshes[i].build(*cams[i], m_calib.geometry, BlendQuality::Feather) ||
                    !m_meshes[i].upload(f)) {
                    qWarning() << "SurroundView: failed to build warp mesh for" << cams[i]->identity;
                    m_initFailed = true;
                    return;
                }
                if (!m_textures[i].create(f, cams[i]->imageWidth, cams[i]->imageHeight)) {
                    qWarning() << "SurroundView: failed to allocate texture for" << cams[i]->identity;
                    m_initFailed = true;
                    return;
                }
            }
            m_initialized = true;
            fprintf(stderr, "[surroundview] initialized 4 cameras (front/rear/left/right, placeholder calibration)\n");
        }
        if (!m_initialized) return;

        auto *f = QOpenGLContext::currentContext()->functions();
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
    bool m_initialized = false;
    bool m_initFailed = false;
};

} // namespace

SurroundView::SurroundView(QQuickItem *parent) : QQuickFramebufferObject(parent) {}

QVariantList SurroundView::feeds() const {
    QVariantList list;
    for (CameraFeed *feed : m_feeds) list.append(QVariant::fromValue(static_cast<QObject *>(feed)));
    return list;
}

void SurroundView::setFeeds(const QVariantList &feeds) {
    for (int i = 0; i < 4; ++i) {
        QObject::disconnect(m_frameConnections[i]);
        m_feeds[i] = (i < feeds.size()) ? qobject_cast<CameraFeed *>(feeds.at(i).value<QObject *>()) : nullptr;
        if (m_feeds[i]) {
            m_frameConnections[i] = connect(m_feeds[i], &CameraFeed::frameReady, this,
                                             [this]() { update(); });
        }
    }
    emit feedsChanged();
}

QQuickFramebufferObject::Renderer *SurroundView::createRenderer() const {
    return new SurroundViewRenderer();
}
