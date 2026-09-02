#include "mediagraph.h"

#ifdef __linux__

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/media.h>

namespace {

// Read /dev/mediaN's full topology (entities, pads, links, interfaces) with
// the kernel's two-call MEDIA_IOC_G_TOPOLOGY protocol: the first call with
// null array pointers reports the counts, then we size the arrays and call
// again. topology_version bumps if the graph changed between the two calls
// (a hot-plug mid-read); retry a few times if so.
struct Topology {
    QVector<media_v2_entity> entities;
    QVector<media_v2_interface> interfaces;
    QVector<media_v2_pad> pads;
    QVector<media_v2_link> links;
    bool ok = false;
};

Topology readTopology(int fd)
{
    Topology t;
    for (int attempt = 0; attempt < 4; ++attempt) {
        struct media_v2_topology top;
        memset(&top, 0, sizeof(top));
        if (::ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &top) < 0)
            return t; // ioctl unsupported (very old kernel) or device gone
        t.entities.resize(int(top.num_entities));
        t.interfaces.resize(int(top.num_interfaces));
        t.pads.resize(int(top.num_pads));
        t.links.resize(int(top.num_links));
        top.ptr_entities = t.entities.isEmpty() ? 0 : reinterpret_cast<quintptr>(t.entities.data());
        top.ptr_interfaces = t.interfaces.isEmpty() ? 0 : reinterpret_cast<quintptr>(t.interfaces.data());
        top.ptr_pads = t.pads.isEmpty() ? 0 : reinterpret_cast<quintptr>(t.pads.data());
        top.ptr_links = t.links.isEmpty() ? 0 : reinterpret_cast<quintptr>(t.links.data());
        const quint64 wantVersion = top.topology_version;
        if (::ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &top) < 0) {
            if (errno == ENOSPC)
                continue; // graph grew between the two calls — re-read counts
            return t;
        }
        if (top.topology_version == wantVersion) {
            t.ok = true;
            return t;
        }
        // changed under us — loop and re-read
    }
    return t;
}

// major:minor -> "/dev/videoN". /sys/dev/char/<maj>:<min> is a symlink into
// the device tree whose leaf is the node name; its uevent carries DEVNAME.
// This is the one place a sysfs read is correct — turning a kernel devnum
// into its /dev path, not guessing a node from a graph position.
QString devNodeFromMajorMinor(quint32 major, quint32 minor)
{
    QFile ue(QStringLiteral("/sys/dev/char/%1:%2/uevent").arg(major).arg(minor));
    if (ue.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = ue.readAll().split('\n');
        for (const QByteArray &l : lines) {
            if (l.startsWith("DEVNAME=")) {
                QString name = QString::fromLocal8Bit(l.mid(8)).trimmed();
                return name.startsWith(QLatin1Char('/')) ? name
                                                         : QStringLiteral("/dev/") + name;
            }
        }
    }
    return QString();
}

// Walk one /dev/mediaN. Returns the capture node for `vc`, or empty.
QString resolveOnMediaDevice(const QString &mediaPath, int vc, QString *diag)
{
    const int fd = ::open(mediaPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return QString();

    Topology top = readTopology(fd);
    ::close(fd);
    if (!top.ok)
        return QString();

    // Index pads by id, and remember each pad's owning entity + direction.
    QHash<quint32, const media_v2_pad *> padById;
    for (const media_v2_pad &p : top.pads)
        padById.insert(p.id, &p);
    QHash<quint32, const media_v2_entity *> entById;
    for (const media_v2_entity &e : top.entities)
        entById.insert(e.id, &e);

    // The camera source. Match by driver name prefix ("nvp6324 4-0031") rather
    // than the SoC-address-bearing bridge/SHIM names — stable across boards
    // wired the same way, and it anchors the walk to THIS camera.
    quint32 srcEntId = 0;
    for (const media_v2_entity &e : top.entities) {
        if (QString::fromUtf8(e.name).startsWith(QStringLiteral("nvp6324"))) {
            srcEntId = e.id;
            break;
        }
    }
    if (!srcEntId) {
        if (diag) *diag = QStringLiteral("no nvp6324 entity in %1").arg(mediaPath);
        return QString();
    }

    // BFS downstream over DATA links (source pad -> sink pad), collecting every
    // entity reachable from the camera. This is the "resolve by route" step:
    // the SHIM context we pick must actually be fed by this camera, not merely
    // named the same.
    QSet<quint32> reached;
    QVector<quint32> queue;
    reached.insert(srcEntId);
    queue.append(srcEntId);
    for (int qi = 0; qi < queue.size(); ++qi) {
        const quint32 ent = queue[qi];
        for (const media_v2_link &l : top.links) {
            if ((l.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK)
                continue;
            const media_v2_pad *sp = padById.value(l.source_id, nullptr);
            const media_v2_pad *dp = padById.value(l.sink_id, nullptr);
            if (!sp || !dp || sp->entity_id != ent)
                continue;
            if (!reached.contains(dp->entity_id)) {
                reached.insert(dp->entity_id);
                queue.append(dp->entity_id);
            }
        }
    }

    // Among the reachable entities, the SHIM context for this VC is the one
    // whose name ends with "context <vc>" (the context index is the VC's
    // static binding on ti-csi2rx). Suffix match, so no SoC address is baked in.
    const QString wantSuffix = QStringLiteral("context %1").arg(vc);
    quint32 ctxEntId = 0;
    for (const quint32 id : reached) {
        const media_v2_entity *e = entById.value(id, nullptr);
        if (e && QString::fromUtf8(e->name).endsWith(wantSuffix)) {
            ctxEntId = id;
            break;
        }
    }
    if (!ctxEntId) {
        if (diag) *diag = QStringLiteral("no '%1' entity downstream of the camera in %2")
                              .arg(wantSuffix, mediaPath);
        return QString();
    }

    // Follow the context entity's V4L video interface link to its devnode.
    for (const media_v2_link &l : top.links) {
        if ((l.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_INTERFACE_LINK)
            continue;
        if (l.sink_id != ctxEntId)
            continue;
        for (const media_v2_interface &intf : top.interfaces) {
            if (intf.id == l.source_id && intf.intf_type == MEDIA_INTF_T_V4L_VIDEO) {
                const QString node = devNodeFromMajorMinor(intf.devnode.major, intf.devnode.minor);
                if (!node.isEmpty())
                    return node;
            }
        }
    }
    if (diag) *diag = QStringLiteral("context for VC%1 has no V4L video interface").arg(vc);
    return QString();
}

// Last-resort fallback if the topology walk can't run (ioctl unsupported, or
// the entity names changed shape): scan the capture nodes' media-entity names
// for "...ticsi2rx context <vc>". Still resolves by entity name rather than a
// fixed number, but without proving the route — hence a fallback, not primary.
QString resolveByNodeName(int vc)
{
    const QString wantSuffix = QStringLiteral("context %1").arg(vc);
    QDir sys(QStringLiteral("/sys/class/video4linux"));
    const QStringList nodes = sys.entryList(QStringList() << QStringLiteral("video*"), QDir::Dirs);
    for (const QString &n : nodes) {
        QFile nameFile(sys.filePath(n) + QStringLiteral("/name"));
        if (!nameFile.open(QIODevice::ReadOnly))
            continue;
        const QString name = QString::fromLocal8Bit(nameFile.readAll()).trimmed();
        if (name.contains(QStringLiteral("ticsi2rx")) && name.endsWith(wantSuffix))
            return QStringLiteral("/dev/") + n;
    }
    return QString();
}

} // namespace

namespace MediaGraph {

QString resolveCsiCaptureNode(int vc, QString *diag)
{
    if (diag)
        diag->clear();
    if (vc < 0) {
        if (diag) *diag = QStringLiteral("invalid vc %1").arg(vc);
        return QString();
    }

    QDir dev(QStringLiteral("/dev"));
    QStringList medias = dev.entryList(QStringList() << QStringLiteral("media*"),
                                       QDir::System | QDir::Files);
    std::sort(medias.begin(), medias.end());
    QString lastDiag;
    for (const QString &m : medias) {
        QString d;
        const QString node = resolveOnMediaDevice(QStringLiteral("/dev/") + m, vc, &d);
        if (!node.isEmpty())
            return node;
        if (!d.isEmpty())
            lastDiag = d;
    }

    // Topology walk found nothing — try the name-scan fallback.
    const QString fallback = resolveByNodeName(vc);
    if (!fallback.isEmpty())
        return fallback;

    if (diag)
        *diag = lastDiag.isEmpty()
                    ? QStringLiteral("no media device resolves VC%1").arg(vc)
                    : lastDiag;
    return QString();
}

} // namespace MediaGraph

#else // !__linux__

namespace MediaGraph {
QString resolveCsiCaptureNode(int, QString *diag)
{
    if (diag)
        *diag = QStringLiteral("media graph resolution is Linux-only");
    return QString();
}
}

#endif
