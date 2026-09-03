#ifndef WAVE5ENCODER_H
#define WAVE5ENCODER_H

// In-app H.264 recording primitives for the AM67A/J722S Wave5 VPU hardware
// encoder. Qt-free (std + V4L2 only) so it also compiles standalone — see
// beagley-ai/wave5-enc/, whose wave5enc.c bring-up harness this is derived
// from, and DASHCAM.md for the design. Linux-only; the whole header compiles
// away on the macOS/simulated dev build.
//
// Two classes, both driven from one camera's capture thread (see camerafeed.cpp):
//   Wave5Encoder  — feed UYVY frames, get Annex-B H.264 access units back.
//   SegmentWriter — write those to rotating, independently-decodable .h264 files.

#if defined(__linux__) && !defined(ULTIMA_SIMULATE)

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

// Drives the Wave5 V4L2 mem2mem stateful encoder (node resolved by QUERYCAP
// driver == "wave5-enc"). UYVY packed 4:2:2 in (the camera's native format —
// no conversion), H.264 Annex-B out. Not thread-safe: create, start(), submit()
// every frame, and stop() all on the same (capture) thread.
class Wave5Encoder {
public:
    // Emitted once per encoded access unit. `data`/`size` is Annex-B; `keyframe`
    // is true for an IDR (from V4L2_BUF_FLAG_KEYFRAME). Called on the submit()
    // thread, before submit() returns.
    using Sink = std::function<void(const uint8_t *data, size_t size, bool keyframe)>;

    Wave5Encoder() = default;
    ~Wave5Encoder();
    Wave5Encoder(const Wave5Encoder &) = delete;
    Wave5Encoder &operator=(const Wave5Encoder &) = delete;

    // Open + configure + STREAMON. width/height are the camera's frame size,
    // srcStride the capture buffer's bytesperline (both used to lay frames into
    // the encoder's input buffers). Returns false on any setup failure (the
    // encoder is left closed). Safe to call once per instance.
    bool start(int width, int height, int srcStride, int bitrateBps, int gop, Sink sink);

    // Encode one UYVY frame. Reclaims consumed input buffers, copies the frame
    // into a free one, queues it, and drains any ready H.264 to the sink.
    // Returns false only on a fatal encoder error (caller should stop()).
    bool submit(const uint8_t *uyvy, int srcStride, int height);

    // Drain-free clean teardown: STREAMOFF both queues, unmap, close. Critical —
    // an unclean stop can leave the VPU firmware mid-command (see DASHCAM.md).
    void stop();

    bool ok() const { return m_fd >= 0; }

private:
    struct Plane { void *start = nullptr; size_t length = 0; };

    bool setFormats(int width, int height);
    void setControls(int bitrateBps, int gop);
    bool reqbufs();
    void reclaimInputs();          // non-blocking DQBUF of consumed OUTPUT bufs
    void drainOutputs();           // non-blocking DQBUF of ready CAPTURE bufs
    int takeFreeInput();           // -1 if none available

    int m_fd = -1;
    int m_width = 0, m_height = 0;
    int m_encStride = 0;           // encoder OUTPUT bytesperline (granted)
    Sink m_sink;

    static constexpr int kNumOut = 6;   // raw input queue
    static constexpr int kNumCap = 6;   // coded output queue
    Plane m_out[kNumOut];
    Plane m_cap[kNumCap];
    int m_numOut = 0, m_numCap = 0;
    std::vector<int> m_freeIn;     // indices of OUTPUT buffers we may fill
    long m_dropped = 0;            // frames dropped for lack of a free input buf
};

// Writes Annex-B access units to timestamped, size-rotated .h264 files under
// <root>/<YYYY-MM-DD>/<HH-MM-SS>_<label>.h264 (or <root>/unsynced/... when the
// wall clock is implausible — proper RTC handling is DASHCAM.md milestone M3).
// Wave5 emits SPS/PPS only once at stream start, so this caches them and writes
// them at the head of every segment — otherwise a rotated file wouldn't decode.
// Rotation happens on a keyframe once the segment has run >= segSeconds, so
// every file begins at an IDR and is independently playable. Not thread-safe.
class SegmentWriter {
public:
    SegmentWriter(std::string root, std::string label, int segSeconds);
    ~SegmentWriter();

    // One encoded access unit from Wave5Encoder::Sink.
    void write(const uint8_t *data, size_t size, bool keyframe);
    void close();                  // flush + fsync + close the current segment

private:
    void cacheParameterSets(const uint8_t *data, size_t size);
    bool openNewSegment();

    std::string m_root, m_label;
    int m_segSeconds;
    std::FILE *m_file = nullptr;
    double m_segStartMono = 0.0;   // CLOCK_MONOTONIC — immune to wall-clock jumps
    std::vector<uint8_t> m_sps, m_pps;
    bool m_warnedOpenFail = false;
};

#endif // linux && !sim
#endif // WAVE5ENCODER_H
