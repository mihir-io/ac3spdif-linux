// Engine: owns the pipeline's lifecycle.
//
//   virtual sink ──PipeWire──▶ pcmRing ──feed──▶ libavcodec ──▶ spdifRing ──▶ S/PDIF
//   (system audio)                       (drift correction)    (IEC 61937)   (passthrough
//                                                                             or ALSA)
//
// Start-up and teardown happen on a worker thread, never the caller's. A tray
// app cannot afford to block its main thread for the second or two a device
// negotiation can take, and the CLI does not care either way. Callers observe
// progress through on_state and poll status().
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/config.h"
#include "engine/log.h"
#include "engine/pipewire_session.h"

namespace ac3spdif {

enum class EngineState { Idle, Starting, Running, Stopping, Failed };

const char* state_label(EngineState s);
inline bool state_is_active(EngineState s) {
    return s == EngineState::Starting || s == EngineState::Running || s == EngineState::Stopping;
}

// Live account of where the pipeline's delay is, measured rather than
// estimated: every term is a reported buffer size or a current ring level.
struct LatencyBreakdown {
    int capture_frames = 0;       // the capture stream's quantum plus reported delay
    int pcm_ring_frames = 0;      // PCM waiting for the encoder; normally near zero
    int encoder_frames = 0;       // inherent: a frame cannot leave before it is complete
    int output_ring_frames = 0;   // what --prebuffer controls
    int device_frames = 0;        // between the ring and the wire
    int total() const { return capture_frames + pcm_ring_frames + encoder_frames + output_ring_frames + device_frames; }
    static double ms(int frames, int rate) { return rate > 0 ? 1000.0 * frames / rate : 0.0; }
};

struct EngineStatus {
    EngineState state = EngineState::Idle;
    std::string failure;
    std::string input_name, output_name, carrier, format, stream;
    std::string sink_format;       // what the sink node negotiated, once linked
    double seconds_on_wire = 0;
    uint64_t bursts = 0;
    int buffer_bursts = 0;
    int target_bursts = 0;
    double drift_ppm = 0;
    uint64_t underrun_bytes = 0;
    uint64_t capture_overflow_frames = 0;
    uint64_t silence_blocks = 0;
    std::vector<float> input_levels;
    std::vector<float> input_peak_hold;
    int source_channels = 0;
    LatencyBreakdown latency;
};

class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    // Both are invoked on engine threads; front ends marshal as they see fit.
    std::function<void(EngineState)> on_state;
    LogSink on_log;

    // Returns immediately. Watch on_state for the outcome.
    void start();
    // Signals the worker and returns immediately; teardown finishes on the
    // worker thread and lands as a final Idle or Failed state.
    void stop();
    // For callers that must not exit until the devices are restored.
    bool wait_until_stopped(std::chrono::milliseconds timeout);

    EngineStatus status() const;
    EngineState state() const;
    // Per-channel input levels read straight from the capture, for meters
    // that want to run faster than the status snapshot refreshes.
    std::vector<float> live_input_levels() const;
    const Config& config() const { return config_; }

    // The sink a configuration would stream to, without starting anything,
    // so a UI can show device-dependent facts before the engine has ever run.
    static std::optional<NodeInfo> preview_output(const Config& config, const Snapshot& snap);

    struct Pipeline;

private:
    void run();
    void set_state(EngineState state);
    void log(const std::string& message);
    void start_pipeline();
    bool stop_requested() const { return stop_requested_.load(); }

    Config config_;
    mutable std::mutex mutex_;
    EngineStatus status_;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
    std::mutex finished_mutex_;
    std::condition_variable finished_cv_;
    bool finished_ = true;
    std::shared_ptr<Pipeline> pipeline_;   // for live meters while running
};

}  // namespace ac3spdif
