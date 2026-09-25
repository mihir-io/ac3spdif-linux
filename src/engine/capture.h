// System-audio capture: a PipeWire input stream on the monitor of the virtual
// sink (or any other node), delivering interleaved float into the PCM ring.
//
// The process callback runs on PipeWire's realtime thread: it copies into a
// lock-free ring and updates a few atomics. Nothing there allocates, locks or
// logs.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/config.h"
#include "engine/pipewire_session.h"
#include "engine/ring.h"

struct pw_stream;
struct spa_pod;

namespace ac3spdif {

// The channel ids PipeWire uses, for a given captured width.
std::vector<uint32_t> spa_channel_ids(int channels);

class Capture {
public:
    static constexpr int kMaxMeteredChannels = 16;

    Capture(PwSession& session, const Config& config, ByteRing& ring, NodeInfo target,
            bool target_is_sink);
    ~Capture();

    void start();
    void stop();

    // Decayed peak per captured channel, 0...1. Read without synchronisation
    // on purpose: the worst outcome is a meter one block stale.
    std::vector<float> levels() const;
    // Highest level per channel since the run began, never decayed. The
    // decaying meter answers "what is playing now", which is useless once
    // playback has stopped; this answers "did anything ever arrive here".
    std::vector<float> peak_hold() const;

    uint64_t overflow_frames() const { return overflow_frames_.load(std::memory_order_relaxed); }
    uint64_t delivered_frames() const { return delivered_frames_.load(std::memory_order_relaxed); }
    int quantum_frames() const { return quantum_frames_.load(std::memory_order_relaxed); }
    // What PipeWire reports between the source and this stream, in frames.
    int64_t delay_frames();
    int source_channels() const { return channels_; }
    std::string target_name() const { return target_.display_name(); }
    bool failed() const { return failed_.load(); }
    std::string error() const;

    // Callbacks, public for the C trampolines.
    void on_process();
    void on_state_changed(int old_state, int state, const char* error);

private:
    PwSession& session_;
    Config config_;
    ByteRing& ring_;
    NodeInfo target_;
    bool target_is_sink_;
    int channels_;
    pw_stream* stream_ = nullptr;
    // A spa_hook by value would drag the whole PipeWire header set into every
    // user of this class; keep it behind a pointer instead.
    void* listener_ = nullptr;
    std::atomic<uint64_t> overflow_frames_{0};
    std::atomic<uint64_t> delivered_frames_{0};
    std::atomic<int> quantum_frames_{0};
    std::array<std::atomic<float>, kMaxMeteredChannels> peak_{};
    std::array<std::atomic<float>, kMaxMeteredChannels> hold_{};
    std::atomic<bool> failed_{false};
    std::string error_;
};

}  // namespace ac3spdif
