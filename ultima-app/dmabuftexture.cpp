#include "dmabuftexture.h"
#include "camerafeed.h"

#include <QDebug>
#include <stdio.h>

#ifdef __linux__
#include <dlfcn.h>

namespace {

// Copied from EGL/eglext.h, GLES2/gl2ext.h and drm/drm_fourcc.h (they're
// ABI, not API) so this builds without EGL headers, and libEGL is dlopen'd
// rather than linked — the macOS dev build has neither, and Qt already
// guarantees libEGL.so.1 is loaded whenever we're actually on eglfs.
constexpr int EGL_WIDTH_ = 0x3057, EGL_HEIGHT_ = 0x3056, EGL_NONE_ = 0x3038;
constexpr int EGL_LINUX_DMA_BUF_EXT_ = 0x3270, EGL_LINUX_DRM_FOURCC_EXT_ = 0x3271;
constexpr int EGL_DMA_BUF_PLANE0_FD_EXT_ = 0x3272, EGL_DMA_BUF_PLANE0_OFFSET_EXT_ = 0x3273;
constexpr int EGL_DMA_BUF_PLANE0_PITCH_EXT_ = 0x3274;
constexpr int EGL_YUV_COLOR_SPACE_HINT_EXT_ = 0x327B, EGL_SAMPLE_RANGE_HINT_EXT_ = 0x327C;
constexpr int EGL_ITU_REC601_EXT_ = 0x327F, EGL_YUV_NARROW_RANGE_EXT_ = 0x3283;
constexpr unsigned GL_TEXTURE_EXTERNAL_OES_ = 0x8D65;
constexpr int DRM_FORMAT_UYVY_ = int('U' | ('Y' << 8) | ('V' << 16) | (unsigned('Y') << 24));

struct Egl {
    bool tried = false, ok = false;
    void *(*getCurrentDisplay)() = nullptr;
    void *(*getProcAddress)(const char *) = nullptr;
    int (*getError)() = nullptr;
    void *(*createImageKHR)(void *, void *, unsigned, void *, const int *) = nullptr;
    unsigned (*destroyImageKHR)(void *, void *) = nullptr;
    void (*imageTargetTexture2DOES)(unsigned, void *) = nullptr;

    bool init() {
        if (tried)
            return ok;
        tried = true;
        void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!lib)
            return false; // not an EGL platform (e.g. a desktop-GL Linux dev build)
        getCurrentDisplay = reinterpret_cast<void *(*)()>(dlsym(lib, "eglGetCurrentDisplay"));
        getProcAddress = reinterpret_cast<void *(*)(const char *)>(dlsym(lib, "eglGetProcAddress"));
        getError = reinterpret_cast<int (*)()>(dlsym(lib, "eglGetError"));
        if (!getCurrentDisplay || !getProcAddress || !getError)
            return false;
        createImageKHR = reinterpret_cast<void *(*)(void *, void *, unsigned, void *, const int *)>(
            getProcAddress("eglCreateImageKHR"));
        destroyImageKHR = reinterpret_cast<unsigned (*)(void *, void *)>(
            getProcAddress("eglDestroyImageKHR"));
        imageTargetTexture2DOES = reinterpret_cast<void (*)(unsigned, void *)>(
            getProcAddress("glEGLImageTargetTexture2DOES"));
        ok = createImageKHR && destroyImageKHR && imageTargetTexture2DOES;
        return ok;
    }
};
Egl g_egl; // touched from the render thread(s) only

} // namespace

bool DmaBufTextureSet::available()
{
    return g_egl.init();
}

#else // !__linux__

bool DmaBufTextureSet::available() { return false; }

#endif

DmaBufTextureSet::~DmaBufTextureSet()
{
    // GL objects need a current context to free — renderers call destroy()
    // from their own destructor when one is available (same conservative
    // guard SurroundTexture users apply). Buffer references can and must
    // go back regardless: CameraFeed outlives every renderer (they're
    // created in main() before the QML engine).
    releaseHeld();
}

void DmaBufTextureSet::releaseHeld()
{
    if (!m_feed)
        return;
    if (m_prev >= 0)
        m_feed->releaseBuffer(m_prev, m_session);
    if (m_display >= 0)
        m_feed->releaseBuffer(m_display, m_session);
    m_prev = m_display = -1;
}

void DmaBufTextureSet::destroyTextures(QOpenGLFunctions *f)
{
#ifdef __linux__
    for (Tex &t : m_tex) {
        if (t.image && g_egl.ok)
            g_egl.destroyImageKHR(g_egl.getCurrentDisplay(), t.image);
        if (t.tex && f)
            f->glDeleteTextures(1, &t.tex);
        t = Tex();
    }
#else
    Q_UNUSED(f);
#endif
}

void DmaBufTextureSet::destroy(QOpenGLFunctions *f)
{
    destroyTextures(f);
    releaseHeld();
}

void DmaBufTextureSet::sync(CameraFeed *feed, QOpenGLFunctions *f)
{
    if (feed != m_feed) {
        releaseHeld();
        destroyTextures(f);
        m_feed = feed;
        m_gen = 0;
        m_session = 0;
    }
    if (!feed || !feed->zeroCopy())
        return;

    const quint32 liveSession = feed->bufferSession();
    if (liveSession != m_session) {
        // Stream restarted (or ended): every index/fd/EGLImage we hold is
        // from the old stream. Drop them now, while the context is current.
        m_display = m_prev = -1; // old-session refs are moot; releaseBuffer would ignore them
        destroyTextures(f);
        m_session = liveSession;
        m_importFailed = false;
    }

    quint32 gen = m_gen, session = 0;
    int idx = feed->acquireLatestBuffer(&gen, &session);
    if (idx < 0)
        return;
    if (m_prev >= 0)
        m_feed->releaseBuffer(m_prev, m_session);
    m_prev = m_display;
    m_display = idx;
    m_gen = gen;
    m_session = session;
    m_fd[idx] = feed->dmabufFd(idx);
    m_width = feed->captureWidth();
    m_height = feed->captureHeight();
    m_stride = feed->captureStride();
}

bool DmaBufTextureSet::bind(QOpenGLFunctions *f, int textureUnit)
{
#ifdef __linux__
    if (m_display < 0 || m_importFailed || !g_egl.init())
        return false;
    Tex &t = m_tex[m_display];
    const int fd = m_fd[m_display];
    if (t.fd != fd || t.session != m_session) {
        if (t.image)
            g_egl.destroyImageKHR(g_egl.getCurrentDisplay(), t.image);
        if (t.tex)
            f->glDeleteTextures(1, &t.tex);
        t = Tex();
        const int attribs[] = {
            EGL_WIDTH_, m_width, EGL_HEIGHT_, m_height,
            EGL_LINUX_DRM_FOURCC_EXT_, DRM_FORMAT_UYVY_,
            EGL_DMA_BUF_PLANE0_FD_EXT_, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT_, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT_, m_stride,
            // The N4 emits BT.601 limited-range (see the media graph's
            // ycbcr:601/quantization:lim-range) — hint the sampler so the
            // GPU decode matches what the camera actually sends.
            EGL_YUV_COLOR_SPACE_HINT_EXT_, EGL_ITU_REC601_EXT_,
            EGL_SAMPLE_RANGE_HINT_EXT_, EGL_YUV_NARROW_RANGE_EXT_,
            EGL_NONE_ };
        t.image = g_egl.createImageKHR(g_egl.getCurrentDisplay(), nullptr,
                                       unsigned(EGL_LINUX_DMA_BUF_EXT_), nullptr, attribs);
        if (!t.image) {
            fprintf(stderr, "[dmabuftexture] eglCreateImageKHR(UYVY %dx%d pitch %d fd %d) failed, eglError=0x%x — this stream won't display zero-copy\n",
                    m_width, m_height, m_stride, fd, g_egl.getError());
            m_importFailed = true; // logged once; retried on the next stream
            return false;
        }
        f->glGenTextures(1, &t.tex);
        f->glBindTexture(GL_TEXTURE_EXTERNAL_OES_, t.tex);
        g_egl.imageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES_, t.image);
        f->glTexParameteri(GL_TEXTURE_EXTERNAL_OES_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_EXTERNAL_OES_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_EXTERNAL_OES_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_EXTERNAL_OES_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t.fd = fd;
        t.session = m_session;
    }
    f->glActiveTexture(GL_TEXTURE0 + textureUnit);
    f->glBindTexture(GL_TEXTURE_EXTERNAL_OES_, t.tex);
    return true;
#else
    Q_UNUSED(f);
    Q_UNUSED(textureUnit);
    return false;
#endif
}
