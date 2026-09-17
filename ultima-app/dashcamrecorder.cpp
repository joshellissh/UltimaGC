#include "dashcamrecorder.h"
#include "camerafeed.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

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

    // Fires only during a format: polls dvrReady() once a second after mkfs so
    // "succeeded" is declared when the drive has actually fsck'd + mounted (the
    // udev mount is async), not merely when mkfs exited 0.
    m_formatWaitTimer.setInterval(1000);
    connect(&m_formatWaitTimer, &QTimer::timeout, this, &DashcamRecorder::onFormatWaitTick);
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
    if (ready != m_recording) {
        m_recording = ready;
        emit recordingChanged();

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

    // Offer-to-format detection runs only when no format flow is active (a flow
    // manages the dialog itself) and no debug/test hook is pinning the dialog.
    if (m_formatState == QLatin1String("idle") && !m_debugOverride)
        updateUnformattedDrive();
}

// ---- format-candidate detection ------------------------------------------

bool DashcamRecorder::deviceHasMount(const QString &name) const
{
    QFile m(QStringLiteral("/proc/mounts"));
    if (!m.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = m.readAll();
    m.close();
    const QByteArray needle = QByteArrayLiteral("/dev/") + name.toUtf8();
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith(needle))
            continue;
        // Only "/dev/sda", "/dev/sda1", "/dev/nvme0n1p1"-style — not a longer
        // name that merely shares the prefix.
        const char c = line.size() > needle.size() ? line.at(needle.size()) : ' ';
        if (c == ' ' || (c >= '0' && c <= '9') || c == 'p')
            return true;
    }
    return false;
}

QString DashcamRecorder::detectFormatCandidate() const
{
#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
    // A correctly-formatted DVR drive carries the ULTIMA_DVR label; udev's
    // blkid creates this symlink as soon as it probes the drive — before our
    // fsck+mount finishes. If it exists, a good drive is present (mounting, or
    // broken), so never offer to wipe. This is what closes the race where a
    // just-plugged *correct* drive would otherwise look "unformatted" during
    // its fsck.
    if (::access("/dev/disk/by-label/ULTIMA_DVR", F_OK) == 0)
        return QString();

    QDir blk(QStringLiteral("/sys/block"));
    blk.setNameFilters(QStringList{ QStringLiteral("sd*") });   // USB mass storage
    blk.setFilter(QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    const QStringList names = blk.entryList();

    QString found;
    int count = 0;
    // Every USB device hangs under a USB *bus* node (usb1, usb2, …) in sysfs —
    // e.g. .../f920000.usb/31200000.usb/xhci-hcd.5.auto/usb1/1-1/.../block/sda
    // (verified on this board 2026-09-17). Match "/usb" + a digit, NOT a bare
    // "/usb/" (there is none — the controller nodes are "*.usb"), and NOT plain
    // "usb" (the ".usb" controller dirs would false-match the disk's absence).
    static const QRegularExpression usbBus(QStringLiteral("/usb[0-9]"));
    for (const QString &name : names) {
        // Core safety guard: the device's sysfs path must traverse a USB bus.
        // This is what excludes the SD card (mmcblk / SDHCI) and everything else
        // on the SoC — only a real USB mass-storage disk qualifies for a wipe.
        // The format helper re-checks this independently.
        const QString real =
            QFileInfo(QStringLiteral("/sys/block/") + name).canonicalFilePath();
        if (!real.contains(usbBus))
            continue;
        // Skip a card reader with no media (reports size 0).
        QFile sz(QStringLiteral("/sys/block/") + name + QStringLiteral("/size"));
        qlonglong sectors = 0;
        if (sz.open(QIODevice::ReadOnly)) {
            sectors = sz.readAll().trimmed().toLongLong();
            sz.close();
        }
        if (sectors <= 0)
            continue;
        // Don't offer to wipe a drive that is currently mounted (in use).
        if (deviceHasMount(name))
            continue;
        found = QStringLiteral("/dev/") + name;
        ++count;
    }
    // Exactly one qualifying USB disk (the board's documented "only removable
    // disk" invariant, see 99-ultima-dvr.rules) → candidate; zero or several →
    // refuse to guess which to wipe.
    return count == 1 ? found : QString();
#else
    return QString();
#endif
}

void DashcamRecorder::updateUnformattedDrive()
{
    const QString raw = detectFormatCandidate();
    if (raw.isEmpty()) {
        m_debCandidate.clear();
        m_debCount = 0;
        setUnformattedDrive(QString());       // clear immediately on unplug
        return;
    }
    if (raw == m_unformattedDrive) {          // already committed
        m_debCandidate.clear();
        m_debCount = 0;
        return;
    }
    // Debounce a new candidate over 2 consecutive polls so we never beat udev's
    // blkid to the ULTIMA_DVR label on a correctly-formatted drive still being
    // probed (which would offer to wipe the good drive).
    if (raw == m_debCandidate) {
        ++m_debCount;
    } else {
        m_debCandidate = raw;
        m_debCount = 1;
    }
    if (m_debCount >= 2)
        setUnformattedDrive(raw);
}

// ---- format flow ----------------------------------------------------------

void DashcamRecorder::formatDrive()
{
    if (m_formatState != QLatin1String("idle"))
        return;                               // already running / showing a result
    if (m_unformattedDrive.isEmpty())
        return;

#if defined(__linux__) && !defined(ULTIMA_SIMULATE)
    // Only run the real, destructive helper for a genuine detected candidate —
    // never when a debug/test hook is pinning the dialog (its "drive" is a
    // placeholder, not a device). Debug-forced prompts fall through to the
    // simulated flow below so the FORMAT button is still demoable on hardware.
    if (!m_debugOverride) {
        setFormatMessage(QString());
        setFormatState(QStringLiteral("formatting"));

        m_fmtProc = new QProcess(this);
        // Merge stderr into stdout so the helper's one-line failure reason is
        // captured regardless of which stream it used.
        m_fmtProc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_fmtProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &DashcamRecorder::onFormatFinished);
        m_fmtProc->start(QStringLiteral("/usr/bin/ultima-dvr-format"),
                         QStringList{ m_unformattedDrive });

        // Hard cap: mkfs on a stick is seconds, but never let a hung helper pin
        // the dialog on "formatting" forever. kill() drives onFormatFinished.
        QTimer::singleShot(120000, this, [this]() {
            if (m_fmtProc && m_formatState == QLatin1String("formatting"))
                m_fmtProc->kill();
        });
        return;
    }
#endif
    // Dev build (no real device) or a debug-forced prompt: simulate a brief
    // format so the flow can be exercised/screenshotted without touching any
    // device. The debug hook can also force explicit end states directly.
    setFormatMessage(QString());
    setFormatState(QStringLiteral("formatting"));
    QTimer::singleShot(1500, this, [this]() {
        if (m_formatState != QLatin1String("formatting"))
            return;
        setFormatMessage(QStringLiteral("Drive ready — recording will start."));
        setFormatState(QStringLiteral("succeeded"));
    });
}

void DashcamRecorder::onFormatFinished(int exitCode, QProcess::ExitStatus status)
{
    QString out;
    if (m_fmtProc) {
        out = QString::fromUtf8(m_fmtProc->readAll()).trimmed();
        m_fmtProc->deleteLater();
        m_fmtProc = nullptr;
    }

    if (status != QProcess::NormalExit || exitCode != 0) {
        // Surface the helper's last line (its fail() reason) to the dialog.
        const QString reason = out.section('\n', -1).trimmed();
        setFormatMessage(reason.isEmpty() ? QStringLiteral("Format failed.") : reason);
        setFormatState(QStringLiteral("failed"));
        return;
    }

    // mkfs + udev re-trigger succeeded; the actual mount (fsck + systemd-mount
    // via the udev rule) is async, so wait for the drive to really come up
    // before declaring success rather than trusting mkfs's exit code alone.
    m_formatWaitMs = 0;
    m_formatWaitTimer.start();
}

void DashcamRecorder::onFormatWaitTick()
{
    m_formatWaitMs += m_formatWaitTimer.interval();
    if (dvrReady()) {
        m_formatWaitTimer.stop();
        setFormatMessage(QStringLiteral("Drive ready — recording has started."));
        setFormatState(QStringLiteral("succeeded"));
        return;
    }
    if (m_formatWaitMs >= 25000) {
        m_formatWaitTimer.stop();
        setFormatMessage(QStringLiteral("Formatted, but the drive did not mount."));
        setFormatState(QStringLiteral("failed"));
    }
}

void DashcamRecorder::dismissFormatPrompt()
{
    if (m_formatState == QLatin1String("formatting"))
        return;                               // can't dismiss mid-format
    m_debugOverride = false;                  // resume normal detection
    if (m_formatState != QLatin1String("idle")) {
        setFormatMessage(QString());
        setFormatState(QStringLiteral("idle"));
    }
    m_promptDismissed = true;
    emit formatPromptOpenChanged();
}

void DashcamRecorder::debugForceState(const QString &state, const QString &drive,
                                      const QString &message)
{
    m_debugOverride = true;                   // pin the dialog against detection
    setUnformattedDrive(drive);               // (also resets m_promptDismissed)
    setFormatMessage(message);
    setFormatState(state);
}

// ---- property setters -----------------------------------------------------

bool DashcamRecorder::formatPromptOpen() const
{
    if (m_formatState != QLatin1String("idle"))
        return true;                          // formatting / result always shown
    return !m_unformattedDrive.isEmpty() && !m_promptDismissed;
}

void DashcamRecorder::setUnformattedDrive(const QString &dev)
{
    if (dev == m_unformattedDrive)
        return;
    m_unformattedDrive = dev;
    // A drive appearing / changing / disappearing invalidates a prior dismissal
    // so a freshly-inserted drive always re-prompts.
    m_promptDismissed = false;
    emit unformattedDriveChanged();
    emit formatPromptOpenChanged();
}

void DashcamRecorder::setFormatState(const QString &s)
{
    if (s == m_formatState)
        return;
    m_formatState = s;
    emit formatStateChanged();
    emit formatPromptOpenChanged();
}

void DashcamRecorder::setFormatMessage(const QString &m)
{
    if (m == m_formatMessage)
        return;
    m_formatMessage = m;
    emit formatMessageChanged();
}
