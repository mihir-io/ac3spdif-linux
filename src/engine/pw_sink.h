// The PipeWire sinks: a passthrough stream in the codec's IEC 61937 format, or
// a plain 16-bit stereo stream used as a carrier.
//
// Passthrough is the real thing. The sink switches into encoded mode, the
// non-audio channel-status bit goes up, and nothing between this stream and the
// hardware can mix, resample or scale the burst. Exclusive by nature: while the
// stream is linked, the sink accepts nothing else.
//
// The PCM carrier writes the same bytes into an ordinary stereo stream. That
// only survives if the volume is unity end to end and nothing resamples, so the
// sink's levels are forced for the run and put back afterwards. The non-audio
// bit is never set, so the receiver has to spot the preamble by itself.
#pragma once

#include <atomic>
#include <optional>
#include <string>

#include "engine/config.h"
#include "engine/log.h"
#include "engine/pipewire_session.h"
#include "engine/ring.h"
#include "engine/sink.h"

struct pw_stream;
struct spa_pod;

namespace ac3spdif {

class PwSink : public BitstreamSink {
public:
    PwSink(PwSession& session, const Config& config, ByteRing& ring, NodeInfo sink,
           bool pcm_carrier, LogSink log);
    ~PwSink() override;

    void prepare() override;
    void begin() override;
    void stop() override;

    uint64_t rendered_bytes() const override { return rendered_.load(std::memory_order_relaxed); }
    uint64_t underrun_bytes() const override { return underrun_.load(std::memory_order_relaxed); }
    double bytes_per_second() const override { return config_.sample_rate * 4.0; }
    std::string device_name() const override { return sink_.display_name(); }
    std::string carrier_label() const override { return pcm_carrier_ ? "pcm" : "passthrough"; }
    std::string format_label() const override;
    std::string stream_label() const override { return "node " + sink_.name; }
    bool is_pcm_carrier() const override { return pcm_carrier_; }
    int request_frames() const override { return request_frames_.load(std::memory_order_relaxed); }
    int64_t device_delay_frames() override;
    bool failed() const override { return failed_.load(); }
    std::string error() const override { return error_; }

    // What the sink node reports as its negotiated format once linked.
    std::string sink_format();
    uint32_t sink_node_id() const { return sink_.id; }

    void on_process();
    void on_state_changed(int old_state, int state, const char* error);
    void on_param_changed(uint32_t id, const spa_pod* param);

private:
    void wait_for_state(int wanted, const char* what);

    PwSession& session_;
    Config config_;
    ByteRing& ring_;
    NodeInfo sink_;
    bool pcm_carrier_;
    LogSink log_;
    pw_stream* stream_ = nullptr;
    void* listener_ = nullptr;
    std::optional<RouteInfo> saved_levels_;
    std::string format_label_;
    std::atomic<uint64_t> rendered_{0};
    std::atomic<uint64_t> underrun_{0};
    std::atomic<int> request_frames_{0};
    std::atomic<bool> failed_{false};
    std::string error_;
};

}  // namespace ac3spdif
