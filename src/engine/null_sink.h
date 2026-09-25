// Encodes normally and throws the bitstream away, pacing the discard at the
// real wire rate so buffer and drift figures stay meaningful.
//
// This exists so the capture half can be examined, channel levels, channel
// mapping, what a source is actually sending, without taking control of an
// audio device. Debugging a silent speaker should not require stopping
// whatever is using the hardware, and pointing a bitstream at an analog output
// to "just have a look" is how people blow up speakers.
#pragma once

#include <atomic>
#include <thread>

#include "engine/ring.h"
#include "engine/sink.h"

namespace ac3spdif {

class NullSink : public BitstreamSink {
public:
    NullSink(ByteRing& ring, int sample_rate) : ring_(ring), bytes_per_second_(sample_rate * 4.0) {}
    ~NullSink() override { stop(); }
    void prepare() override {}
    void begin() override;
    void stop() override;
    uint64_t rendered_bytes() const override { return rendered_.load(); }
    uint64_t underrun_bytes() const override { return 0; }
    double bytes_per_second() const override { return bytes_per_second_; }
    std::string device_name() const override { return "none (diagnostic)"; }
    std::string carrier_label() const override { return "discard"; }
    std::string format_label() const override { return "encoded, then discarded"; }
    std::string stream_label() const override { return "no device claimed, nothing reaches hardware"; }
    bool is_pcm_carrier() const override { return false; }
    int request_frames() const override { return 0; }
    int64_t device_delay_frames() override { return 0; }
    bool failed() const override { return false; }
    std::string error() const override { return ""; }

private:
    ByteRing& ring_;
    double bytes_per_second_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> rendered_{0};
};

}  // namespace ac3spdif
