#include "shadermanager.h"

#include <QFile>
#include <QDebug>
#include <QStringList>
#include <QOpenGLContext>

namespace {
// GLSL ES requires #extension directives to appear before any
// non-preprocessor token — but versionHeader() unconditionally prepends
// "#version ..." + precision statements ahead of the shader file body, and
// precision statements are themselves non-preprocessor tokens. A shader
// that needs an extension (e.g. a future yuv_external.frag's
// GL_OES_EGL_image_external_essl3) can't just put "#extension" at the top
// of its own file and have that work once concatenated. So: pull any
// #extension lines leading the file body out, and the caller re-inserts
// them between "#version" and the precision block instead — the one
// ordering every driver accepts.
QString takeLeadingExtensionLines(QString &body) {
    QString extLines;
    QStringList lines = body.split(QLatin1Char('\n'));
    int i = 0;
    for (; i < lines.size(); ++i) {
        QString trimmed = lines[i].trimmed();
        if (trimmed.startsWith(QLatin1String("#extension"))) {
            extLines += lines[i] + QLatin1Char('\n');
        } else if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("//"))) {
            continue; // blank lines / comments don't end the leading run
        } else {
            break;
        }
    }
    if (!extLines.isEmpty()) {
        // Only actually drop the #extension lines themselves — leave
        // comments/blank lines before them in place, they're harmless.
        for (int j = 0; j < i; ++j) {
            if (lines[j].trimmed().startsWith(QLatin1String("#extension"))) lines[j].clear();
        }
        body = lines.join(QLatin1Char('\n'));
    }
    return extLines;
}
} // namespace

QString ShaderManager::versionHeader(QOpenGLShader::ShaderType type, const QString &extensionLines) const {
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    bool isGLES = ctx && ctx->isOpenGLES();
    if (isGLES) {
        QString h = QStringLiteral("#version 300 es\n") + extensionLines;
        if (type == QOpenGLShader::Fragment) h += QStringLiteral("precision highp float;\nprecision highp sampler2D;\n");
        return h;
    }
    // Desktop GL (macOS dev build) has no equivalent of the GLES-only
    // extensions this project needs — extensionLines is silently dropped
    // here rather than emitted as desktop #extension syntax, since no
    // caller of this class currently needs one on the desktop dialect.
    return QStringLiteral("#version 330 core\n");
}

QString ShaderManager::loadSource(const QString &resourcePath) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "ShaderManager: could not open" << resourcePath;
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

QOpenGLShaderProgram *ShaderManager::program(const QString &vertPath, const QString &fragPath,
                                             Variant variant) {
    std::string key = (vertPath + "|" + fragPath + (variant == ExternalSampler ? "|ext" : "")).toStdString();
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    QString vertBody = loadSource(vertPath);
    QString fragBody = loadSource(fragPath);
    if (vertBody.isEmpty() || fragBody.isEmpty()) return nullptr;

    QString fragExtensionLines = takeLeadingExtensionLines(fragBody);
    if (variant == ExternalSampler) {
        fragBody.replace(QLatin1String("uniform sampler2D uTexture;"),
                         QLatin1String("uniform samplerExternalOES uTexture;"));
        fragExtensionLines += QStringLiteral("#extension GL_OES_EGL_image_external_essl3 : require\n");
    }

    auto prog = std::make_unique<QOpenGLShaderProgram>();
    QString vertSrc = versionHeader(QOpenGLShader::Vertex) + vertBody;
    QString fragSrc = versionHeader(QOpenGLShader::Fragment, fragExtensionLines) + fragBody;

    if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vertSrc)) {
        qWarning() << "ShaderManager: vertex compile failed for" << vertPath << ":" << prog->log();
        return nullptr;
    }
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc)) {
        qWarning() << "ShaderManager: fragment compile failed for" << fragPath << ":" << prog->log();
        return nullptr;
    }
    if (!prog->link()) {
        qWarning() << "ShaderManager: link failed for" << vertPath << "+" << fragPath << ":" << prog->log();
        return nullptr;
    }

    QOpenGLShaderProgram *raw = prog.get();
    m_cache.emplace(key, std::move(prog));
    return raw;
}
