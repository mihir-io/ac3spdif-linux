// The encode stage, in-process through libavcodec, and the drift controller
// that keeps two independent audio clocks in step.
//
// Clock drift is the subtle problem in this pipeline. The capture side and the
// S/PDIF device may be clocked by different oscillators, so one is always
// slightly faster. A normal audio path fixes that by resampling, but a
// bitstream cannot be resampled: every byte of an IEC 61937 burst is load
// bearing. So the correction happens upstream, in the PCM domain, before
// anything is encoded: the feed thread always encodes exactly one frame's
// worth of samples but varies how many samples it *consumes* to build it.
// Consuming 1537 to emit 1536 sheds a sample; consuming 1535 and repeating the
// last one adds one. A single dropped or duplicated sample at 48 kHz is
// inaudible.
//
// libavformat's spdif muxer does the IEC 61937 framing, exactly as the ffmpeg
// command line would, so a dumped stream decodes with `ffmpeg -f spdif`.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/config.h"
#include "engine/log.h"
#include "engine/ring.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVIOContext;
struct SwrContext;
struct AVFrame;
struct AVPacket;

namespace ac3spdif {

struct EncoderStats {
    uint64_t blocks_fed = 0;
    uint64_t samples_dropped = 0;
    uint64_t samples_duplicated = 0;
    uint64_t silence_blocks = 0;
    uint64_t burst_bytes = 0;
};

class Encoder {
public:
    Encoder(const Config& config, ByteRing& pcm_ring, ByteRing& spdif_ring);
    ~Encoder();

    void start();
    void stop();
    bool running() const { return running_.load(); }
    std::string last_error() const;
    EncoderStats stats() const;
    LogSink on_log;

    int block_frames() const { return block_frames_; }
    int burst_bytes() const { return burst_bytes_; }

    // Samples to add or remove from one block, given how far the output ring
    // is from its target in bytes. Public so a unit test can pin it down.
    int correction_frames(long error_bytes) const;
    int max_correction_frames() const;

private:
    void open_codec();
    void close_codec();
    void feed_loop();
    bool encode_block(const float* interleaved);
    size_t read_blocking(uint8_t* destination, size_t offset, size_t byte_count);
    int on_write(const uint8_t* data, int size);
    void fail(const std::string& message);

    Config config_;
    ByteRing& pcm_ring_;
    ByteRing& spdif_ring_;
    int block_frames_;
    int burst_bytes_;
    long target_fill_;
    long deadband_;

    AVCodecContext* codec_ctx_ = nullptr;
    AVFormatContext* format_ctx_ = nullptr;
    AVIOContext* avio_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int64_t next_pts_ = 0;
    int dump_fd_ = -1;

    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    EncoderStats stats_;
    std::string last_error_;
};

}  // namespace ac3spdif
