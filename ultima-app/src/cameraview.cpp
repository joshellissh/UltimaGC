#include "cameraview.h"
#include "shadermanager.h"
#include "surroundtexture.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QDebug>

namespace {

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
        // guarantee a current context at Renderer destruction.
        if (!m_initialized || !QOpenGLContext::currentContext()) return;
        m_texture.destroy(QOpenGLContext::currentContext()->functions());
        m_vbo.destroy();
        m_vao.destroy();
    }

    // Called on the render thread with the GUI thread blocked — same
    // "safe without a lock" guarantee CameraFeed::currentFrame()'s own
    // comment relies on, mirrors SurroundViewRenderer::synchronize().
    void synchronize(QQuickFramebufferObject *fboItem) override {
        auto *item = static_cast<CameraView *>(fboItem);
        CameraFeed *feed = item->feed();
        if (!feed) return;
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

        if (m_pendingValid) {
            m_texture.upload(f, m_pendingImage);
            m_pendingValid = false;
        }

        m_vao.bind();
        m_prog->bind();
        m_prog->setUniformValue("uTexture", 0);
        m_texture.bind(f, 0);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_prog->release();
        m_vao.release();
    }

    ShaderManager m_shaders;
    QOpenGLShaderProgram *m_prog = nullptr;
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
    m_feed = feed;
    if (m_feed) {
        m_frameConnection = connect(m_feed, &CameraFeed::frameReady, this, [this]() {
            update();
        });
    }
    emit feedChanged();
    update();
}

QQuickFramebufferObject::Renderer *CameraView::createRenderer() const
{
    return new CameraViewRenderer();
}
