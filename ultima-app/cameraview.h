#ifndef CAMERAVIEW_H
#define CAMERAVIEW_H

#include <QQuickFramebufferObject>
#include <QMetaObject>
#include <QString>

#include "camerafeed.h"

// Renders one CameraFeed's latest frame, un-stitched, into a fullscreen quad
// — CameraGridScreen's plain 2x2 raw view (Camera360Screen's stitched view
// is SurroundView, a separate class with its own warp mesh).
//
// Was a QQuickPaintedItem (QPainter::drawImage() into a FramebufferObject
// render target) until real-hardware profiling (2026-08-17, see
// beagleplay-falcon/NOTES.md and test/avm-benchmark/docs/measurement-notes.md)
// found CameraGridScreen running at 2-3 FPS on the real BeaglePlay/PowerVR
// target: strace on the render thread showed it ~90% saturated, dominated by
// ioctl (39%) + mmap (17%) — the GPU driver allocating a brand-new texture
// every single paint, because QPainter's GL paint engine keys its texture
// cache on QImage::cacheKey(), which changes every call CameraFeed hands it
// a freshly-converted frame (by design — detach() bumps it unconditionally,
// see QImage's own source, so even reusing one QImage's identity in place
// would not have avoided this). QQuickFramebufferObject sidesteps the whole
// problem by owning a persistent GL_TEXTURE_2D (SurroundTexture, the same
// class SurroundView already uses) and updating it via glTexSubImage2D each
// frame instead — an in-place update, not a reallocation.
//
// Deliberately QQuickFramebufferObject, not a hand-managed QSGSimpleTextureNode:
// SurroundView already established (see its own class comment, and
// test/avm-benchmark's "QQuickFramebufferObject on the real PowerVR/eglfs
// target" section) that QQuickFramebufferObject never hands texture/FBO
// lifetime to application code, which is what avoided the two hardware-
// verified QSGTexture crashes (SIGSEGV/SIGBUS) this project hit on this
// exact target before CameraView became a QQuickPaintedItem in the first
// place. This change keeps that guarantee — it swaps QQuickPaintedItem's
// costly per-frame QPainter/texture-cache path for QQuickFramebufferObject's
// own equally crash-safe, but non-reallocating, path.
class CameraView : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(CameraFeed *feed READ feed WRITE setFeed NOTIFY feedChanged)
    // "" (default) = raw passthrough, blit.vert/blit.frag, unchanged from
    // before this property existed — what CameraGridScreen wants, so it
    // never sets this. "left"/"right" = reproject through mirror.frag
    // instead, using the matching placeholder yaw/pitch/roll/FOV constants
    // in cameraview.cpp (see that file for where the numbers came from —
    // found empirically against a hand-sketched target-FOV diagram, not a
    // real measured mount). Any other value falls back to raw passthrough.
    Q_PROPERTY(QString mirrorViewSide READ mirrorViewSide WRITE setMirrorViewSide NOTIFY mirrorViewSideChanged)

public:
    explicit CameraView(QQuickItem *parent = nullptr);

    CameraFeed *feed() const { return m_feed; }
    void setFeed(CameraFeed *feed);

    QString mirrorViewSide() const { return m_mirrorViewSide; }
    void setMirrorViewSide(const QString &side);

    Renderer *createRenderer() const override;

protected:
    // Don't build the FBO node (renderer + FBO + shader compiles) for a
    // view that isn't visible yet — see cameraview.cpp.
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;

signals:
    void feedChanged();
    void mirrorViewSideChanged();

private:
    CameraFeed *m_feed = nullptr;
    QMetaObject::Connection m_frameConnection;
    // Also update() on streamingChanged: a stream ending must still run one
    // sync so the renderer's DmaBufTextureSet can drop its EGLImages (see
    // dmabuftexture.h's session note) — frameReady alone stops firing
    // exactly when that matters.
    QMetaObject::Connection m_streamConnection;
    QString m_mirrorViewSide;
};

#endif
