#ifndef DMABUFTEXTURE_H
#define DMABUFTEXTURE_H

#include <QOpenGLFunctions>
#include <QtGlobal>

class CameraFeed;

// Zero-copy display of a CameraFeed's V4L2 capture buffers (path 1 in
// camerafeed.h's class comment): each buffer's dma-buf fd is imported once
// per stream as an EGLImage bound to a GL_TEXTURE_EXTERNAL_OES texture, and
// per frame the render pass just binds the texture for whichever buffer is
// newest. Nothing is converted or uploaded by the CPU — the GPU's sampler
// reads the UYVY directly (and applies the BT.601 limited-range transfer
// the camera actually emits, which the CPU path approximates as full-range).
//
// Hardware-verified 2026-08-26 on the target's PowerVR (Mesa-based DDK
// 25.2, EGL_EXT_image_dma_buf_import + GL_OES_EGL_image_external_essl3):
// with 4 grid quadrants at 1080p25 the render thread sits at ~22% where the
// glTexSubImage2D upload path saturated it at ~9.5fps — see
// GAUGE-CLUSTER.md "Camera framerate".
//
// One instance per (renderer, feed). Every method runs on the render
// thread: sync() during the scene-graph sync phase (GUI thread blocked, GL
// context current), bind() during render, destroy() whenever a GL context
// is current for teardown. Holds the CURRENT and PREVIOUS buffer index —
// the previous frame's render has completed by the time the next sync runs
// (the swap blocks on vsync), so nothing the GPU might still be scanning
// ever goes back to the driver's DMA queue.
class DmaBufTextureSet {
public:
    ~DmaBufTextureSet();

    // Pick up the feed's newest buffer, if any. Also detects a stream
    // restart (bufferSession() bump) and drops every imported texture from
    // the old stream right here — the GL context is current during sync,
    // and freeing promptly is what keeps stale EGLImages from pinning CMA
    // memory the driver needs for the next stream's REQBUFS. (If a hidden
    // view misses that window, tryOpen()'s reconnect backoff retries the
    // allocation a second later — self-healing, just noisier.)
    void sync(CameraFeed *feed, QOpenGLFunctions *f);

    bool hasFrame() const { return m_display >= 0; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Binds the current buffer's external texture to textureUnit,
    // importing it first if this buffer hasn't been seen this stream.
    // false = nothing to draw (no frame yet, or the import failed —
    // logged once per stream).
    bool bind(QOpenGLFunctions *f, int textureUnit);

    void destroy(QOpenGLFunctions *f);

    // Whether the EGL entry points this needs exist at all. False on
    // desktop GL (the macOS dev build) — callers fall back to the
    // converted-QImage texture path.
    static bool available();

private:
    static constexpr int kMaxBuffers = 8; // mirrors CameraFeed::kMaxBuffers
    struct Tex {
        int fd = -1;
        quint32 session = 0;
        void *image = nullptr;  // EGLImageKHR
        GLuint tex = 0;
    };
    Tex m_tex[kMaxBuffers];
    int m_fd[kMaxBuffers] = {-1, -1, -1, -1, -1, -1, -1, -1};
    CameraFeed *m_feed = nullptr;
    int m_display = -1, m_prev = -1;
    quint32 m_gen = 0, m_session = 0;
    int m_width = 0, m_height = 0, m_stride = 0;
    bool m_importFailed = false;
    void releaseHeld();
    void destroyTextures(QOpenGLFunctions *f);
};

#endif
