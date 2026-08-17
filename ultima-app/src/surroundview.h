#ifndef SURROUNDVIEW_H
#define SURROUNDVIEW_H

#include <QQuickFramebufferObject>
#include <QVariantList>
#include <QMetaObject>

#include "camerafeed.h"
#include "calibrationstore.h"

// Top-down 360°/birds-eye compositor: warps each of 4 CameraFeeds through a
// precomputed WarpMesh (fisheye + ground-plane correction, done once, never
// per-frame — see warpmesh.h) and feather-blends them into one canvas.
//
// Architecture ported from test/avm-benchmark's real-hardware-validated
// design (see that project's docs/measurement-notes.md): a
// QQuickFramebufferObject — the same GPU-backed-Qt-Quick-item pattern this
// project already uses for CameraView (see cameraview.h's class comment on
// the two hardware-verified crashes a hand-managed QSGTexture approach hit
// on this exact BeaglePlay/PowerVR target) — rendering synchronously on the
// render thread rather than a second GL context/thread (the benchmark's
// TEST 20 measured an async variant as *worse* on this non-preemptive GPU,
// not better).
//
// Unlike the benchmark's own QQuickFramebufferObject item (which polled a
// synthetic 30Hz clock against a 60Hz display clock and needed a due-check
// + continuously-rearmed update() to stay correctly throttled), this one is
// purely event-driven: real camera frames arrive via CameraFeed::frameReady
// at whatever rate the driver delivers them, each one calling update()
// directly. No self-driven due-check/re-arm loop is needed — the next
// update() only ever happens when a real new frame does.
//
// feeds[] index -> camera identity is currently a PLACEHOLDER assumption
// (cam1=front, cam2=rear, cam3=left, cam4=right, matching
// defaultCalibration()'s CalibrationSet.front/rear/left/right order) — this
// has not been confirmed against how the 4 physical cameras are actually
// wired/mounted on the car. If the stitched view's quadrants land in the
// wrong place, check this mapping before suspecting the mesh math.
class SurroundView : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList feeds READ feeds WRITE setFeeds NOTIFY feedsChanged)
    Q_PROPERTY(CalibrationStore *calibrationSource READ calibrationSource WRITE setCalibrationSource NOTIFY calibrationSourceChanged)

public:
    explicit SurroundView(QQuickItem *parent = nullptr);

    QVariantList feeds() const;
    void setFeeds(const QVariantList &feeds);

    CalibrationStore *calibrationSource() const { return m_calibrationSource; }
    void setCalibrationSource(CalibrationStore *source);

    // Index 0..3 = front/rear/left/right — see class comment above. Used by
    // SurroundViewRenderer::synchronize() to read each feed's current frame.
    CameraFeed *feedAt(int index) const { return (index >= 0 && index < 4) ? m_feeds[index] : nullptr; }

    // Consumed by SurroundViewRenderer::synchronize() each sync — returns
    // true (and clears the flag) at most once per calibrationChanged().
    bool consumeCalibrationDirty() { bool d = m_calibrationDirty; m_calibrationDirty = false; return d; }
    CalibrationSet currentCalibration() const { return m_calibrationSource ? m_calibrationSource->toSet() : CalibrationSet(); }

    Renderer *createRenderer() const override;

signals:
    void feedsChanged();
    void calibrationSourceChanged();

private:
    CameraFeed *m_feeds[4] = {nullptr, nullptr, nullptr, nullptr};
    QMetaObject::Connection m_frameConnections[4];
    CalibrationStore *m_calibrationSource = nullptr;
    QMetaObject::Connection m_calibrationConnection;
    bool m_calibrationDirty = false;
};

#endif
