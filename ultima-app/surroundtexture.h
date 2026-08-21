#ifndef SURROUNDTEXTURE_H
#define SURROUNDTEXTURE_H

#include <QOpenGLFunctions>
#include <QImage>

// One GL_TEXTURE_2D holding a camera's current frame, uploaded from a
// QImage (whatever format CameraFeed::currentFrame() hands back — RGB32
// today) via glTexSubImage2D. This is the portable path: plain RGBA8,
// works identically on macOS desktop GL and the target's GLES, no EGL/
// DMA-BUF/platform-specific code at all — see test/avm-benchmark's TEST 17
// "Leg A" for why this is the correctness-first baseline to build on
// before a native zero-copy import path (Leg C in that project) is worth
// adding here.
class SurroundTexture {
public:
    bool create(QOpenGLFunctions *f, int width, int height);
    void destroy(QOpenGLFunctions *f);

    // glTexSubImage2D upload of image, converted to a known R,G,B,A byte
    // order first if it isn't already — QImage::Format_RGB32/ARGB32 store
    // pixels as a native-endian 0xAARRGGBB word, which is NOT the same
    // byte order as GL_RGBA on a little-endian machine (it's GL_BGRA);
    // Format_RGBA8888 is the one QImage format Qt guarantees is R,G,B,A in
    // memory on every platform, so converting to it here (rather than
    // assuming a GL_BGRA extension is present on every target) is what
    // keeps this correct without a platform-specific format assumption.
    // image's pixel size must match what create() was called with.
    bool upload(QOpenGLFunctions *f, const QImage &image);

    void bind(QOpenGLFunctions *f, int textureUnit);

    unsigned int textureId() const { return m_tex; }

private:
    unsigned int m_tex = 0;
    int m_width = 0, m_height = 0;
};

#endif
