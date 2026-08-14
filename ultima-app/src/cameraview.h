#ifndef CAMERAVIEW_H
#define CAMERAVIEW_H

#include <QQuickPaintedItem>
#include <QMetaObject>

#include "camerafeed.h"

// Renders CameraFeed's latest frame.
//
// Originally a QQuickItem with a hand-managed QSGSimpleTextureNode
// (window()->createTextureFromImage() + manual delete of the old texture
// each frame) — reverted after two hardware-verified crashes on the real
// BeaglePlay/PowerVR target (2026-08-14, see beagleplay-falcon/NOTES.md
// "Live camera feed"): a SIGSEGV in the manual delete, and a SIGBUS deep in
// libQt5Core.so after switching to deleteLater(). Neither reproduced on the
// macOS dev build, which never exercises the real eglfs_kms/PowerVR
// scenegraph path at all. QQuickPaintedItem hands texture/FBO lifetime
// entirely to Qt — no manual QSGTexture create/delete in application code —
// which removes that whole failure class rather than chasing it further on
// a target with no symbolized Qt libraries to debug against.
class CameraView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(CameraFeed *feed READ feed WRITE setFeed NOTIFY feedChanged)

public:
    explicit CameraView(QQuickItem *parent = nullptr);

    CameraFeed *feed() const { return m_feed; }
    void setFeed(CameraFeed *feed);

    void paint(QPainter *painter) override;

signals:
    void feedChanged();

private:
    CameraFeed *m_feed = nullptr;
    QMetaObject::Connection m_frameConnection;
};

#endif
