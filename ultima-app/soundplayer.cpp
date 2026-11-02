#include "soundplayer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef ULTIMA_ALSA
#include <alsa/asoundlib.h>
#endif

namespace {

struct Note {
    float hz;   // 0 = silence
    int ms;
};
using Notes = std::vector<Note>;

// The sound *names* the dash plays (grouped by who fires them, see
// soundevents.cpp), each with a built-in tone. The tones are only the fallback:
// a <name>.wav in soundDirs() (below) replaces it, and that is the intended way
// to supply the real sounds. Keep sounds short — they play over whatever else
// is going on in the car.
const QHash<QString, Notes> &library()
{
    static const QHash<QString, Notes> lib = [] {
        QHash<QString, Notes> l;

        // Turn signal / hazard relay: tick on the lit phase, a lower tock on
        // the dark phase (the dash synthesises the blink, see main.qml).
        l.insert("tick", { {2200, 14} });
        l.insert("tock", { {1700, 14} });

        // Warnings. "alarm" is the urgent one (oil pressure, coolant) and is
        // repeated by SoundEvents while the fault persists; "warn" is a
        // single falling two-tone for everything else.
        l.insert("alarm", { {1300, 140}, {0, 80}, {1300, 140}, {0, 80}, {1300, 140} });
        l.insert("warn",  { {880, 120}, {0, 30}, {660, 200} });

        // Drive mode: the number of beeps says which mode you landed in
        // (VALET 1, SPORT 2, SPORT+ 3 rising, RACE 4 fast) — no need to look.
        l.insert("mode_valet",     { {600, 100} });
        l.insert("mode_sport",     { {800, 80}, {0, 70}, {800, 80} });
        l.insert("mode_sportplus", { {800, 70}, {0, 50}, {1000, 70}, {0, 50}, {1200, 70} });
        l.insert("mode_race",      { {1400, 55}, {0, 45}, {1400, 55}, {0, 45}, {1400, 55}, {0, 45}, {1400, 55} });

        // Dashcam. Recording started rises, stopped falls; the "format this
        // drive?" prompt is a double ping; success/error close the flow.
        l.insert("rec_start", { {660, 90}, {880, 90}, {1100, 160} });
        l.insert("rec_stop",  { {1100, 90}, {880, 90}, {660, 160} });
        l.insert("prompt",    { {988, 100}, {0, 60}, {988, 100} });
        l.insert("success",   { {784, 90}, {1047, 220} });
        l.insert("error",     { {330, 120}, {0, 30}, {220, 300} });

        return l;
    }();
    return lib;
}

#ifdef ULTIMA_ALSA

// What the McASP -> IT66122 HDMI card accepts: S32_LE stereo at 32/48 kHz
// (S16 and 44.1 kHz are rejected on hw:; the "default" PCM's plug layer would
// convert, but rendering natively avoids relying on that).
constexpr unsigned kRate = 48000;
constexpr int kChunkMs = 20;          // silence-feed granularity (= worst-case start latency on top of the ALSA buffer)
constexpr int kLeadInMs = 250;        // silent lead-in on a freshly opened stream (HDMI sink relock)
constexpr int kIdleCloseMs = 4000;    // keep the stream alive this long after the last sound
constexpr unsigned kLatencyUs = 80000;
constexpr qint64 kRetryOpenMs = 2000; // don't hammer a missing card

// Mono sine notes -> interleaved stereo S32_LE with a short attack/release on
// each note so nothing clicks.
std::vector<int32_t> render(const Notes &notes, float volume)
{
    std::vector<int32_t> pcm;
    const double amp = 2147483647.0 * 0.95 * std::min(1.0f, std::max(0.0f, volume));
    const double twoPi = 6.283185307179586;
    for (const Note &n : notes) {
        const int frames = int(int64_t(kRate) * n.ms / 1000);
        const int attack = std::min(frames / 4, int(kRate) * 3 / 1000);
        const int release = std::min(frames / 3, int(kRate) * 10 / 1000);
        for (int i = 0; i < frames; ++i) {
            double env = 1.0;
            if (attack > 0 && i < attack)
                env = double(i) / attack;
            else if (release > 0 && i >= frames - release)
                env = double(frames - i) / release;
            const double s = n.hz > 0 ? std::sin(twoPi * n.hz * i / kRate) * env : 0.0;
            const int32_t v = int32_t(s * amp);
            pcm.push_back(v);   // L
            pcm.push_back(v);   // R
        }
    }
    return pcm;
}

// --- WAV loading ----------------------------------------------------------
//
// Real recordings are the intended sound source; the synthesised library()
// above is only the fallback for a name with no file. Accepts what editors
// export: RIFF/WAVE, PCM 8/16/24/32-bit or 32-bit float (plain or
// WAVE_FORMAT_EXTENSIBLE), mono or multichannel (first two used), any sample
// rate. Everything is converted to the stream format (48 kHz interleaved
// stereo S32_LE, linear-interpolated resample) and scaled by the software
// volume. Exporting at 48 kHz avoids the resample entirely.
std::vector<int32_t> loadWav(const QString &path, float volume, QString &err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024) {
        err = QStringLiteral("cannot open (or larger than 16 MB)");
        return {};
    }
    const QByteArray d = f.readAll();
    auto u16 = [&](qint64 o) { return unsigned(uchar(d[int(o)])) | unsigned(uchar(d[int(o) + 1])) << 8; };
    auto u32 = [&](qint64 o) { return u16(o) | u16(o + 2) << 16; };

    if (d.size() < 12 || memcmp(d.constData(), "RIFF", 4) != 0 || memcmp(d.constData() + 8, "WAVE", 4) != 0) {
        err = QStringLiteral("not a RIFF/WAVE file");
        return {};
    }
    unsigned tag = 0, ch = 0, rate = 0, bits = 0;
    qint64 dataOff = -1, dataLen = 0;
    qint64 pos = 12;
    while (pos + 8 <= d.size()) {
        const unsigned sz = u32(pos + 4);
        const qint64 body = pos + 8;
        if (memcmp(d.constData() + pos, "fmt ", 4) == 0 && sz >= 16 && body + 16 <= d.size()) {
            tag = u16(body);
            ch = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (tag == 0xFFFE && sz >= 26 && body + 26 <= d.size())
                tag = u16(body + 24);       // EXTENSIBLE: real format is the SubFormat GUID's first word
        } else if (memcmp(d.constData() + pos, "data", 4) == 0) {
            dataOff = body;
            dataLen = std::min<qint64>(sz, d.size() - body);
            break;
        }
        pos = body + sz + (sz & 1);          // chunks are word-aligned
    }
    if (dataOff < 0 || rate == 0 || ch == 0) {
        err = QStringLiteral("missing fmt or data chunk");
        return {};
    }
    const bool isPcm = tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool isFloat = tag == 3 && bits == 32;
    if (!isPcm && !isFloat) {
        err = QStringLiteral("unsupported format (tag %1, %2-bit)").arg(tag).arg(bits);
        return {};
    }
    const qint64 bytesPer = bits / 8;
    const qint64 frameSize = bytesPer * ch;
    const qint64 frames = dataLen / frameSize;
    if (frames <= 0) {
        err = QStringLiteral("no audio data");
        return {};
    }

    const uchar *base = reinterpret_cast<const uchar *>(d.constData()) + dataOff;
    auto sample = [&](qint64 frame, unsigned c) -> float {
        const uchar *p = base + frame * frameSize + qint64(c) * bytesPer;
        switch (bits) {
        case 8:  return (float(p[0]) - 128.0f) / 128.0f;
        case 16: return float(int16_t(p[0] | p[1] << 8)) / 32768.0f;
        case 24: {
            int32_t v = int32_t(p[0] | p[1] << 8 | p[2] << 16);
            if (v & 0x800000)
                v |= ~0xFFFFFF;
            return float(v) / 8388608.0f;
        }
        default:
            if (isFloat) {
                float v;
                memcpy(&v, p, 4);
                return v;
            }
            return float(int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24))
                   / 2147483648.0f;
        }
    };

    const unsigned right = ch >= 2 ? 1 : 0;   // mono -> both channels
    const qint64 outFrames = std::max<qint64>(1, frames * kRate / rate);
    const double gain = 2147483647.0 * std::min(1.0f, std::max(0.0f, volume));
    auto toS32 = [&](double x) {
        return int32_t(std::min(2147483647.0, std::max(-2147483647.0, x * gain)));
    };
    std::vector<int32_t> pcm;
    pcm.reserve(size_t(outFrames) * 2);
    for (qint64 i = 0; i < outFrames; ++i) {
        const double src = double(i) * rate / kRate;
        const qint64 i0 = std::min<qint64>(frames - 1, qint64(src));
        const qint64 i1 = std::min<qint64>(frames - 1, i0 + 1);
        const double fr = src - double(i0);
        const double l = sample(i0, 0) * (1.0 - fr) + sample(i1, 0) * fr;
        const double r = sample(i0, right) * (1.0 - fr) + sample(i1, right) * fr;
        pcm.push_back(toS32(l));
        pcm.push_back(toS32(r));
    }
    return pcm;
}

// Where <name>.wav is looked for, first hit wins. /data/sounds is the
// writable partition, so a sound can be swapped on a running car without a
// rebuild; /usr/share/ultima/sounds is what the image ships (the recipe
// installs ultima-app/assets/sounds/*.wav there).
QStringList soundDirs()
{
    QStringList dirs;
    const QString env = qEnvironmentVariable("ULTIMA_SOUND_DIR");
    if (!env.isEmpty())
        dirs << env;
    dirs << QStringLiteral("/data/sounds") << QStringLiteral("/usr/share/ultima/sounds");
    return dirs;
}

// PCM for `name`: its .wav if there is a usable one, else the built-in tone.
std::vector<int32_t> loadSound(const QString &name, float volume)
{
    for (const QString &dir : soundDirs()) {
        const QString path = dir + QLatin1Char('/') + name + QStringLiteral(".wav");
        if (!QFile::exists(path))
            continue;
        QString err;
        std::vector<int32_t> pcm = loadWav(path, volume, err);
        if (!pcm.empty()) {
            fprintf(stderr, "[sound] %s <- %s (%.2f s)\n", qPrintable(name), qPrintable(path),
                    double(pcm.size() / 2) / kRate);
            return pcm;
        }
        fprintf(stderr, "[sound] %s: %s — ignored\n", qPrintable(path), qPrintable(err));
    }
    fprintf(stderr, "[sound] %s: no .wav found, using the built-in tone\n", qPrintable(name));
    return render(library().value(name), volume);
}

snd_pcm_t *openPcm(const QString &device)
{
    snd_pcm_t *pcm = nullptr;
    const QByteArray dev = device.toLocal8Bit();
    int err = snd_pcm_open(&pcm, dev.constData(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[sound] snd_pcm_open(%s) failed: %s\n", dev.constData(), snd_strerror(err));
        return nullptr;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, kRate, 1 /* soft resample */, kLatencyUs);
    if (err < 0) {
        fprintf(stderr, "[sound] snd_pcm_set_params(%s) failed: %s\n", dev.constData(), snd_strerror(err));
        snd_pcm_close(pcm);
        return nullptr;
    }
    return pcm;
}

// Blocking write of interleaved stereo frames, recovering from xruns.
bool writeFrames(snd_pcm_t *pcm, const int32_t *data, size_t frames)
{
    while (frames > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, data, frames);
        if (n < 0) {
            if (snd_pcm_recover(pcm, int(n), 1) < 0)
                return false;
            continue;
        }
        data += size_t(n) * 2;
        frames -= size_t(n);
    }
    return true;
}

void closePcm(snd_pcm_t *&pcm, bool drain)
{
    if (!pcm)
        return;
    if (drain)
        snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    pcm = nullptr;
}

#endif // ULTIMA_ALSA

} // namespace

SoundPlayer::SoundPlayer(QObject *parent)
    : QObject(parent)
{
    m_enabled = qEnvironmentVariable("ULTIMA_SOUND", QStringLiteral("1")) != QLatin1String("0");
    bool ok = false;
    const float vol = qEnvironmentVariable("ULTIMA_SOUND_VOLUME").toFloat(&ok);
    if (ok)
        m_volume = std::min(1.0f, std::max(0.0f, vol));
    m_device = qEnvironmentVariable("ULTIMA_SOUND_DEVICE", QStringLiteral("default"));

#ifdef ULTIMA_ALSA
    if (m_enabled)
        m_thread = std::thread([this] { run(); });
#endif
}

SoundPlayer::~SoundPlayer()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stop = true;
    }
    m_cond.wakeAll();
    if (m_thread.joinable())
        m_thread.join();
}

QStringList SoundPlayer::names()
{
    QStringList l = library().keys();
    l.sort();
    return l;
}

void SoundPlayer::play(const QString &name)
{
    if (!m_enabled)
        return;
    if (!library().contains(name)) {
        fprintf(stderr, "[sound] unknown sound '%s'\n", qPrintable(name));
        return;
    }
#ifdef ULTIMA_ALSA
    {
        QMutexLocker lock(&m_mutex);
        if (m_queue.size() >= 4)
            return;             // backed up: drop rather than play stale
        m_queue.enqueue(name);
    }
    m_cond.wakeOne();
#else
    // macOS / WSL dev builds: no ALSA — just say what would have played.
    if (qEnvironmentVariableIntValue("ULTIMA_SOUND_LOG"))
        fprintf(stderr, "[sound] (no ALSA in this build) %s\n", qPrintable(name));
#endif
}

void SoundPlayer::run()
{
#ifdef ULTIMA_ALSA
    snd_pcm_t *pcm = nullptr;
    QElapsedTimer lastActive;
    QElapsedTimer lastOpenFail;
    QHash<QString, std::vector<int32_t>> cache;
    const std::vector<int32_t> silence(size_t(kRate) * kChunkMs / 1000 * 2, 0);
    const std::vector<int32_t> leadIn(size_t(kRate) * kLeadInMs / 1000 * 2, 0);

    for (;;) {
        QString name;
        {
            QMutexLocker lock(&m_mutex);
            // Stream closed and nothing to play: sleep until a request.
            // (While the stream is open the loop must keep feeding silence,
            // so it never waits.)
            if (!m_stop && m_queue.isEmpty() && !pcm)
                m_cond.wait(&m_mutex);
            if (m_stop)
                break;
            if (!m_queue.isEmpty())
                name = m_queue.dequeue();
        }

        if (!name.isEmpty()) {
            auto it = cache.find(name);
            if (it == cache.end())
                it = cache.insert(name, loadSound(name, m_volume));

            if (!pcm) {
                if (lastOpenFail.isValid() && !lastOpenFail.hasExpired(kRetryOpenMs))
                    continue;   // card still absent; drop this one quietly
                pcm = openPcm(m_device);
                if (!pcm) {
                    lastOpenFail.restart();
                    continue;
                }
                lastOpenFail.invalidate();
                fprintf(stderr, "[sound] opened %s\n", qPrintable(m_device));
                if (!writeFrames(pcm, leadIn.data(), leadIn.size() / 2)) {
                    closePcm(pcm, false);
                    continue;
                }
            }
            if (!writeFrames(pcm, it->data(), it->size() / 2))
                closePcm(pcm, false);
            lastActive.restart();
        } else if (pcm) {
            if (lastActive.hasExpired(kIdleCloseMs))
                closePcm(pcm, true);
            else if (!writeFrames(pcm, silence.data(), silence.size() / 2))
                closePcm(pcm, false);
        }
    }
    closePcm(pcm, false);
#endif
}
