#include "wave5encoder.h"

#if defined(__linux__) && !defined(ULTIMA_SIMULATE)

#include <cstring>
#include <algorithm>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <linux/videodev2.h>

namespace {

constexpr int kOutType = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  // raw UYVY in
constexpr int kCapType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // coded H.264 out

double monoNow()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// mplane QBUF/DQBUF with a single plane (UYVY packed and H.264 are both 1-plane).
int qbuf(int fd, int type, int index, size_t bytesused)
{
    struct v4l2_plane planes[1];
    memset(planes, 0, sizeof(planes));
    planes[0].bytesused = bytesused;
    struct v4l2_buffer v;
    memset(&v, 0, sizeof(v));
    v.type = type;
    v.memory = V4L2_MEMORY_MMAP;
    v.index = index;
    v.length = 1;
    v.m.planes = planes;
    return ::ioctl(fd, VIDIOC_QBUF, &v);
}

// Returns index >= 0, or -1 (errno set; EAGAIN = nothing ready).
int dqbuf(int fd, int type, size_t *bytesused, bool *keyframe)
{
    struct v4l2_plane planes[1];
    memset(planes, 0, sizeof(planes));
    struct v4l2_buffer v;
    memset(&v, 0, sizeof(v));
    v.type = type;
    v.memory = V4L2_MEMORY_MMAP;
    v.length = 1;
    v.m.planes = planes;
    if (::ioctl(fd, VIDIOC_DQBUF, &v) < 0)
        return -1;
    if (bytesused)
        *bytesused = planes[0].bytesused;
    if (keyframe)
        *keyframe = (v.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    return int(v.index);
}

} // namespace

Wave5Encoder::~Wave5Encoder() { stop(); }

bool Wave5Encoder::start(int width, int height, int srcStride, int bitrateBps, int gop, Sink sink)
{
    (void)srcStride;
    if (m_fd >= 0)
        return false;
    m_width = width;
    m_height = height;
    m_sink = std::move(sink);

    // Resolve the encoder node by QUERYCAP driver name, never a fixed number
    // (same discipline as mediagraph.cpp; video0/1 are decoder/encoder by probe
    // order only). See beagley-ai/wave5-enc/README.md.
    int fd = -1;
    for (int i = 0; i < 64 && fd < 0; ++i) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        int t = ::open(p, O_RDWR | O_NONBLOCK);
        if (t < 0)
            continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (::ioctl(t, VIDIOC_QUERYCAP, &cap) == 0 &&
            strcmp(reinterpret_cast<const char *>(cap.driver), "wave5-enc") == 0)
            fd = t;
        else
            ::close(t);
    }
    if (fd < 0) {
        fprintf(stderr, "[wave5enc] no wave5-enc node found (module loaded?)\n");
        return false;
    }
    m_fd = fd;

    if (!setFormats(width, height)) { stop(); return false; }
    setControls(bitrateBps, gop);
    if (!reqbufs()) { stop(); return false; }

    // Queue all coded-output buffers up front.
    for (int i = 0; i < m_numCap; ++i) {
        if (qbuf(m_fd, kCapType, i, 0) < 0) {
            fprintf(stderr, "[wave5enc] QBUF cap[%d]: %s\n", i, strerror(errno));
            stop();
            return false;
        }
    }

    // ORDER IS LOAD-BEARING: STREAMON the capture (coded) queue before the
    // output (raw) queue, or the driver never reaches PIC_RUN and silently
    // encodes nothing (vb2 sets q->streaming after the callback returns; the
    // driver's start_streaming gates seq-init on cap_q already streaming). See
    // DASHCAM.md / wave5-enc/README.md.
    int t = kCapType;
    if (::ioctl(m_fd, VIDIOC_STREAMON, &t) < 0) {
        fprintf(stderr, "[wave5enc] STREAMON cap: %s\n", strerror(errno));
        stop();
        return false;
    }
    t = kOutType;
    if (::ioctl(m_fd, VIDIOC_STREAMON, &t) < 0) {
        fprintf(stderr, "[wave5enc] STREAMON out: %s\n", strerror(errno));
        stop();
        return false;
    }

    fprintf(stderr, "[wave5enc] started %dx%d UYVY->H.264 @%d bps gop %d (%d in / %d out bufs)\n",
            width, height, bitrateBps, gop, m_numOut, m_numCap);
    return true;
}

bool Wave5Encoder::setFormats(int width, int height)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = kOutType;
    f.fmt.pix_mp.width = width;
    f.fmt.pix_mp.height = height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    // Match what the cameras emit (BT.601 limited range) — the encoder captures
    // colorimetry from this OUTPUT format, and the V4L2 encoder path stalls on a
    // mismatch (GAUGE-CLUSTER.md: cameras are smpte170m/601 limited).
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_SMPTE170M;
    f.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
    f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    f.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    if (::ioctl(m_fd, VIDIOC_S_FMT, &f) < 0) {
        fprintf(stderr, "[wave5enc] S_FMT out: %s\n", strerror(errno));
        return false;
    }
    m_encStride = int(f.fmt.pix_mp.plane_fmt[0].bytesperline);

    memset(&f, 0, sizeof(f));
    f.type = kCapType;
    f.fmt.pix_mp.width = width;
    f.fmt.pix_mp.height = height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.num_planes = 1;
    if (::ioctl(m_fd, VIDIOC_S_FMT, &f) < 0) {
        fprintf(stderr, "[wave5enc] S_FMT cap: %s\n", strerror(errno));
        return false;
    }

    // Crop so the SPS signals the real display height (the encoder codes at the
    // 16-px-aligned 1088). Best-effort — cosmetic if the driver rejects it.
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = kOutType;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.left = 0;
    sel.r.top = 0;
    sel.r.width = width;
    sel.r.height = height;
    if (::ioctl(m_fd, VIDIOC_S_SELECTION, &sel) < 0)
        fprintf(stderr, "[wave5enc] S_SELECTION crop (non-fatal): %s\n", strerror(errno));
    return true;
}

void Wave5Encoder::setControls(int bitrateBps, int gop)
{
    struct Ctl { uint32_t id; int32_t val; };
    const Ctl ctls[] = {
        { V4L2_CID_MPEG_VIDEO_BITRATE, bitrateBps },   // default is 0 = CBR w/ no target -> emits nothing
        { V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop },
        { V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, gop },
        { V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2 }, // 1080p-capable; default level 1.0 is non-conformant
    };
    for (const Ctl &c : ctls) {
        struct v4l2_ext_control ec;
        memset(&ec, 0, sizeof(ec));
        ec.id = c.id;
        ec.value = c.val;
        struct v4l2_ext_controls ecs;
        memset(&ecs, 0, sizeof(ecs));
        ecs.which = V4L2_CTRL_WHICH_CUR_VAL;
        ecs.count = 1;
        ecs.controls = &ec;
        if (::ioctl(m_fd, VIDIOC_S_EXT_CTRLS, &ecs) < 0)
            fprintf(stderr, "[wave5enc] S_EXT_CTRLS id=0x%x (non-fatal): %s\n", c.id, strerror(errno));
    }
}

bool Wave5Encoder::reqbufs()
{
    struct v4l2_requestbuffers rb;
    // OUTPUT (raw input)
    memset(&rb, 0, sizeof(rb));
    rb.count = kNumOut;
    rb.type = kOutType;
    rb.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(m_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        fprintf(stderr, "[wave5enc] REQBUFS out: %s\n", strerror(errno));
        return false;
    }
    m_numOut = int(rb.count);
    m_freeIn.clear();
    for (int i = 0; i < m_numOut; ++i) {
        struct v4l2_plane planes[1];
        memset(planes, 0, sizeof(planes));
        struct v4l2_buffer v;
        memset(&v, 0, sizeof(v));
        v.type = kOutType;
        v.memory = V4L2_MEMORY_MMAP;
        v.index = i;
        v.length = 1;
        v.m.planes = planes;
        if (::ioctl(m_fd, VIDIOC_QUERYBUF, &v) < 0) {
            fprintf(stderr, "[wave5enc] QUERYBUF out[%d]: %s\n", i, strerror(errno));
            return false;
        }
        m_out[i].length = planes[0].length;
        m_out[i].start = ::mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, m_fd, planes[0].m.mem_offset);
        if (m_out[i].start == MAP_FAILED) {
            fprintf(stderr, "[wave5enc] mmap out[%d]: %s\n", i, strerror(errno));
            m_out[i].start = nullptr;
            return false;
        }
        m_freeIn.push_back(i);
    }

    // CAPTURE (coded output)
    memset(&rb, 0, sizeof(rb));
    rb.count = kNumCap;
    rb.type = kCapType;
    rb.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(m_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        fprintf(stderr, "[wave5enc] REQBUFS cap: %s\n", strerror(errno));
        return false;
    }
    m_numCap = int(rb.count);
    for (int i = 0; i < m_numCap; ++i) {
        struct v4l2_plane planes[1];
        memset(planes, 0, sizeof(planes));
        struct v4l2_buffer v;
        memset(&v, 0, sizeof(v));
        v.type = kCapType;
        v.memory = V4L2_MEMORY_MMAP;
        v.index = i;
        v.length = 1;
        v.m.planes = planes;
        if (::ioctl(m_fd, VIDIOC_QUERYBUF, &v) < 0) {
            fprintf(stderr, "[wave5enc] QUERYBUF cap[%d]: %s\n", i, strerror(errno));
            return false;
        }
        m_cap[i].length = planes[0].length;
        m_cap[i].start = ::mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, m_fd, planes[0].m.mem_offset);
        if (m_cap[i].start == MAP_FAILED) {
            fprintf(stderr, "[wave5enc] mmap cap[%d]: %s\n", i, strerror(errno));
            m_cap[i].start = nullptr;
            return false;
        }
    }
    return true;
}

void Wave5Encoder::reclaimInputs()
{
    for (;;) {
        int idx = dqbuf(m_fd, kOutType, nullptr, nullptr);
        if (idx < 0)
            break;                 // EAGAIN (or error — harmless here)
        m_freeIn.push_back(idx);
    }
}

int Wave5Encoder::takeFreeInput()
{
    if (m_freeIn.empty())
        return -1;
    int idx = m_freeIn.back();
    m_freeIn.pop_back();
    return idx;
}

void Wave5Encoder::drainOutputs()
{
    for (;;) {
        size_t used = 0;
        bool key = false;
        int idx = dqbuf(m_fd, kCapType, &used, &key);
        if (idx < 0)
            break;                 // EAGAIN: nothing more ready this pass
        if (used > 0 && m_sink)
            m_sink(static_cast<const uint8_t *>(m_cap[idx].start), used, key);
        if (qbuf(m_fd, kCapType, idx, 0) < 0)
            fprintf(stderr, "[wave5enc] re-QBUF cap[%d]: %s\n", idx, strerror(errno));
    }
}

bool Wave5Encoder::submit(const uint8_t *uyvy, int srcStride, int height)
{
    if (m_fd < 0)
        return false;

    reclaimInputs();
    int idx = takeFreeInput();
    if (idx < 0) {
        // Encoder is behind (it normally runs several x real-time, so this is
        // rare). Give it one short poll to free an input rather than dropping.
        struct pollfd pfd = { m_fd, POLLOUT, 0 };
        if (::poll(&pfd, 1, 10) > 0)
            reclaimInputs();
        idx = takeFreeInput();
    }
    if (idx < 0) {
        if ((++m_dropped % 100) == 1)
            fprintf(stderr, "[wave5enc] dropped a frame, encoder busy (total %ld)\n", m_dropped);
        drainOutputs();
        return true;
    }

    uint8_t *dst = static_cast<uint8_t *>(m_out[idx].start);
    const int rowBytes = std::min(srcStride, m_encStride);
    const int rows = std::min(height, m_height);
    for (int r = 0; r < rows; ++r)
        memcpy(dst + size_t(r) * m_encStride, uyvy + size_t(r) * srcStride, size_t(rowBytes));

    if (qbuf(m_fd, kOutType, idx, m_out[idx].length) < 0) {
        fprintf(stderr, "[wave5enc] QBUF out[%d]: %s — stopping\n", idx, strerror(errno));
        return false;
    }
    drainOutputs();
    return true;
}

void Wave5Encoder::stop()
{
    if (m_fd < 0)
        return;
    int t = kOutType;
    ::ioctl(m_fd, VIDIOC_STREAMOFF, &t);
    t = kCapType;
    ::ioctl(m_fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < m_numOut; ++i)
        if (m_out[i].start) { ::munmap(m_out[i].start, m_out[i].length); m_out[i] = Plane(); }
    for (int i = 0; i < m_numCap; ++i)
        if (m_cap[i].start) { ::munmap(m_cap[i].start, m_cap[i].length); m_cap[i] = Plane(); }
    m_numOut = m_numCap = 0;
    m_freeIn.clear();
    ::close(m_fd);
    m_fd = -1;
}

// ------------------------------------------------------------------ SegmentWriter

namespace {

void mkdirp(const std::string &path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1)
            ::mkdir(cur.c_str(), 0755);   // ignore EEXIST
    }
    ::mkdir(path.c_str(), 0755);
}

// Iterate Annex-B NAL units, invoking fn(nalType, start, len) for each (len
// spans the start code through to the next one). 3- and 4-byte start codes.
template <typename F>
void forEachNal(const uint8_t *d, size_t n, F fn)
{
    size_t i = 0;
    long prev = -1;
    int prevType = -1;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (prev >= 0)
                fn(prevType, d + prev, size_t(long(i) - prev));
            prevType = (i + 3 < n) ? (d[i + 3] & 0x1f) : -1;
            prev = long(i);
            i += 3;
        } else {
            ++i;
        }
    }
    if (prev >= 0)
        fn(prevType, d + prev, n - size_t(prev));
}

} // namespace

SegmentWriter::SegmentWriter(std::string root, std::string label, int segSeconds)
    : m_root(std::move(root)), m_label(std::move(label)), m_segSeconds(segSeconds) {}

SegmentWriter::~SegmentWriter() { close(); }

void SegmentWriter::cacheParameterSets(const uint8_t *data, size_t size)
{
    if (!m_sps.empty() && !m_pps.empty())
        return;                    // SPS/PPS never change; cache once
    forEachNal(data, size, [&](int type, const uint8_t *p, size_t len) {
        if (type == 7 && m_sps.empty())
            m_sps.assign(p, p + len);
        else if (type == 8 && m_pps.empty())
            m_pps.assign(p, p + len);
    });
}

bool SegmentWriter::openNewSegment()
{
    close();

    std::time_t now = std::time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char dateDir[32], timeStr[32];
    std::string dir;
    if (tmv.tm_year + 1900 < 2021) {
        // Clock not set yet — keep footage but out of the dated tree. Proper RTC
        // handling is M3 (DASHCAM.md).
        dir = m_root + "/unsynced";
        snprintf(timeStr, sizeof(timeStr), "%ld", (long)monoNow());
    } else {
        strftime(dateDir, sizeof(dateDir), "%Y-%m-%d", &tmv);
        strftime(timeStr, sizeof(timeStr), "%H-%M-%S", &tmv);
        dir = m_root + "/" + dateDir;
    }
    mkdirp(dir);
    std::string path = dir + "/" + timeStr + "_" + m_label + ".h264";
    m_file = std::fopen(path.c_str(), "wb");
    if (!m_file) {
        if (!m_warnedOpenFail) {
            fprintf(stderr, "[dashcam] cannot open segment %s: %s\n", path.c_str(), strerror(errno));
            m_warnedOpenFail = true;
        }
        return false;
    }
    m_warnedOpenFail = false;
    m_segStartMono = monoNow();
    // Prepend cached SPS/PPS so this segment decodes on its own (the encoder
    // sends them only once, at stream start).
    if (!m_sps.empty())
        std::fwrite(m_sps.data(), 1, m_sps.size(), m_file);
    if (!m_pps.empty())
        std::fwrite(m_pps.data(), 1, m_pps.size(), m_file);
    return true;
}

void SegmentWriter::write(const uint8_t *data, size_t size, bool keyframe)
{
    cacheParameterSets(data, size);

    const bool needRotate = keyframe &&
        (m_file == nullptr || (monoNow() - m_segStartMono) >= m_segSeconds);
    if (needRotate) {
        if (!openNewSegment())
            return;                // drive gone / unwritable — drop until it recovers
    }
    if (m_file && std::fwrite(data, 1, size, m_file) != size) {
        // Write failed — almost always the drive was yanked mid-segment. Close
        // now so this handle stops pinning the (about-to-be-unmounted) mount;
        // the recorder's mount poll then stops recording within a few seconds,
        // and the next keyframe re-opens once a drive is back.
        fprintf(stderr, "[dashcam] segment write failed (%s) — closing segment\n", strerror(errno));
        close();
    }
}

void SegmentWriter::close()
{
    if (!m_file)
        return;
    std::fflush(m_file);
    ::fsync(::fileno(m_file));
    std::fclose(m_file);
    m_file = nullptr;
}

#endif // linux && !sim
