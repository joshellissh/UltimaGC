#include "dashcamrecorder.h"
#include "camerafeed.h"

#include <cstdio>
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
#include <sys/stat.h>
#include <unistd.h>
#endif

DashcamRecorder::DashcamRecorder(QVector<CameraFeed *> feeds, QString root,
                                 int bitrateBps, int gop, int segSeconds, QObject *parent)
    : QObject(parent), m_feeds(std::move(feeds)), m_root(std::move(root)),
      m_bitrate(bitrateBps), m_gop(gop), m_segSeconds(segSeconds)
{
    m_timer.setInterval(3000); // detect drive hot-plug/removal within a few seconds
    connect(&m_timer, &QTimer::timeout, this, &DashcamRecorder::poll);
}

void DashcamRecorder::start()
{
    // Arm the poll timer but do NOT poll synchronously here: recording opens the
    // cameras (and their capture threads + encoders), and this project keeps the
    // camera path lazy specifically to protect boot-time-to-first-frame. The
    // first poll therefore lands one interval (~3s) after the event loop starts,
    // by which point the dash is up — a dashcam missing the first few seconds
    // after power-on is fine. Hot-plug/removal is then caught every interval.
    m_timer.start();
}

bool DashcamRecorder::dvrReady() const
{
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
    // /mnt/dvr is a real mount only if its device differs from its parent's;
    // the mountpoint dir itself is baked into the rootfs (ultima-dvr-mount), so
    // st_dev equality means "nothing mounted there yet".
    struct stat here, parent;
    if (::stat("/mnt/dvr", &here) != 0 || ::stat("/mnt", &parent) != 0)
        return false;
    if (here.st_dev == parent.st_dev)
        return false;
    return ::access("/mnt/dvr", W_OK) == 0;
#else
    return false;    // no real drive on the dev build
#endif
}

void DashcamRecorder::poll()
{
    const bool ready = dvrReady();
    if (ready == m_recording)
        return;      // no transition
    m_recording = ready;

    for (CameraFeed *f : m_feeds) {
        if (!f)
            continue;
        if (ready) {
            f->configureRecording(m_root, m_bitrate, m_gop, m_segSeconds);
            f->setRecording(true);
        } else {
            f->setRecording(false);
        }
    }
    fprintf(stderr, "[dashcam] recording %s (DVR drive %s)\n",
            ready ? "ENABLED" : "disabled",
            ready ? "mounted at /mnt/dvr" : "absent");
}
