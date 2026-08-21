#include "surroundtexture.h"

bool SurroundTexture::create(QOpenGLFunctions *f, int width, int height) {
    m_width = width;
    m_height = height;

    f->glGenTextures(1, &m_tex);
    f->glBindTexture(GL_TEXTURE_2D, m_tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return m_tex != 0;
}

void SurroundTexture::destroy(QOpenGLFunctions *f) {
    if (m_tex) { f->glDeleteTextures(1, &m_tex); m_tex = 0; }
}

bool SurroundTexture::upload(QOpenGLFunctions *f, const QImage &image) {
    if (!m_tex) return false;
    if (image.width() != m_width || image.height() != m_height) return false;

    QImage rgba = image.format() == QImage::Format_RGBA8888
                      ? image
                      : image.convertToFormat(QImage::Format_RGBA8888);

    f->glBindTexture(GL_TEXTURE_2D, m_tex);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba.constBits());
    return true;
}

void SurroundTexture::bind(QOpenGLFunctions *f, int textureUnit) {
    f->glActiveTexture(GL_TEXTURE0 + textureUnit);
    f->glBindTexture(GL_TEXTURE_2D, m_tex);
}
