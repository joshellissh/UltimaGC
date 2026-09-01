#include "camerafeed.h"

#include <QPainter>
#include <QFont>

static constexpr int kPlaceholderWidth = 1280;
static constexpr int kPlaceholderHeight = 720;

CameraFeed::CameraFeed(const QString &label, QObject *parent)
    : QObject(parent), m_label(label)
{
}

CameraFeed::~CameraFeed()
{
}

void CameraFeed::setActive(bool on)
{
    if (on == m_active)
        return;
    m_active = on;
    emit activeChanged();

    if (m_active) {
        setFailed(false);
        showPlaceholder();
    } else {
        setStreaming(false);
    }
}

void CameraFeed::setStreaming(bool on)
{
    if (on == m_streaming)
        return;
    m_streaming = on;
    emit streamingChanged();
}

void CameraFeed::setFailed(bool on)
{
    if (on == m_failed)
        return;
    m_failed = on;
    emit failedChanged();
}

// See the class comment: ULTIMA_CAM_IMAGE_DIR wins if set and decodable,
// otherwise a plain drawn card labeled with this feed's name. Loaded/drawn
// once per instance and cached — reactivating a feed just redisplays it.
void CameraFeed::showPlaceholder()
{
    if (!m_placeholderLoaded) {
        m_placeholderLoaded = true;
        const QString dir = qEnvironmentVariable("ULTIMA_CAM_IMAGE_DIR");
        if (!dir.isEmpty()) {
            QImage img(dir + QLatin1Char('/') + m_label + QStringLiteral(".png"));
            if (!img.isNull())
                m_placeholder = img;
        }
        if (m_placeholder.isNull()) {
            QImage card(kPlaceholderWidth, kPlaceholderHeight, QImage::Format_RGB32);
            card.fill(QColor(24, 24, 28));
            QPainter p(&card);
            p.setPen(QColor(90, 90, 96));
            p.drawRect(20, 20, card.width() - 40, card.height() - 40);
            p.setPen(Qt::white);
            QFont font = p.font();
            font.setPointSize(40);
            p.setFont(font);
            p.drawText(card.rect(), Qt::AlignCenter,
                       QStringLiteral("NO CAMERA\n%1").arg(m_label));
            p.end();
            m_placeholder = card;
        }
    }

    if (m_frameWidth != m_placeholder.width() || m_frameHeight != m_placeholder.height()) {
        m_frameWidth = m_placeholder.width();
        m_frameHeight = m_placeholder.height();
        emit formatChanged();
    }
    m_frame = m_placeholder;
    setStreaming(true);
    emit frameReady();
}
