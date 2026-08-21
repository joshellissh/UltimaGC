#ifndef SHADERMANAGER_H
#define SHADERMANAGER_H

#include <QOpenGLShaderProgram>
#include <QString>
#include <memory>
#include <unordered_map>
#include <string>

// Compiles/links/caches QOpenGLShaderProgram instances from .vert/.frag
// resource paths, prepending the right #version + precision header for
// whichever GL dialect is actually current (desktop GL on macOS dev
// builds, GLES on the real BeaglePlay/eglfs target) — callers write plain
// GLSL body text with no #version line of their own.
//
// Ported from test/avm-benchmark/src/graphics/ShaderManager.{h,cpp}, with
// one simplification: that project threaded a GLContext wrapper through
// just to answer "GLES or desktop GL" for a headless/offscreen benchmark
// harness with several context-creation paths. This project's GL code only
// ever runs inside Qt Quick's own scene graph context, so
// QOpenGLContext::currentContext()->isOpenGLES() answers the same question
// directly with no wrapper needed.
class ShaderManager {
public:
    // vertPath/fragPath are qrc:-less resource paths, e.g. ":/shaders/surround.vert".
    // Returns nullptr (and logs via qWarning) on compile/link failure.
    // Cached by (vertPath, fragPath) pair — repeat calls with the same pair
    // return the same program.
    QOpenGLShaderProgram *program(const QString &vertPath, const QString &fragPath);

private:
    QString versionHeader(QOpenGLShader::ShaderType type, const QString &extensionLines = QString()) const;
    static QString loadSource(const QString &resourcePath);

    std::unordered_map<std::string, std::unique_ptr<QOpenGLShaderProgram>> m_cache;
};

#endif
