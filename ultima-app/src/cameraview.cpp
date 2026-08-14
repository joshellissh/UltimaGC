#include "cameraview.h"

#include <QPainter>

CameraView::CameraView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    // FramebufferObject, not the ImageBackend default: the default target
    // paints into a QImage on the CPU and then uploads that as a texture
    // every frame — a second CPU copy on top of paint()'s own drawImage().
    // FramebufferObject paints straight into a GPU-backed FBO via
    // QPainter's GL paint engine, which matters here since this project has
    // the PowerVR GPU enabled specifically for this (see
    // beagleplay-falcon/NOTES.md "PowerVR GPU enablement").
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

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

void CameraView::paint(QPainter *painter)
{
    if (!m_feed)
        return;

    QImage image = m_feed->currentFrame();
    if (image.isNull())
        return; // nothing new yet — leave whatever was already painted

    painter->drawImage(boundingRect(), image);
}
