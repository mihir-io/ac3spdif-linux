#include "engine/engine.h"

#include <algorithm>
#include <cmath>

#include "engine/alsa_devices.h"
#include "engine/alsa_sink.h"
#include "engine/capture.h"
#include "engine/encoder.h"
#include "engine/null_sink.h"
#include "engine/pw_sink.h"
#include "engine/ring.h"
#include "engine/selftest.h"

namespace ac3spdif {

const char* state_label(EngineState s) {
    switch (s) {
        case EngineState::Idle: return "Stopped";
        case EngineState::Starting: return "Starting…";
        case EngineState::Running: return "Streaming";
        case EngineState::Stopping: return "Stopping…";
        case EngineState::Failed: return "Failed";
    }
    return "?";
}

// Everything that lives for one run, torn down in reverse order of creation.
struct Engine::Pipeline {
    std::unique_ptr<PwSession> session;
    std::unique_ptr<ByteRing> pcm_ring;
    std::unique_ptr<ByteRing> spdif_ring;
    std::unique_ptr<Encoder> encoder;
    std::unique_ptr<Capture> capture;
    std::unique_ptr<ToneGenerator> generator;
    std::unique_ptr<BitstreamSink> sink;
    std::optional<NodeInfo> virtual_sink;
    std::optional<NodeInfo> output_node;
    bool created_virtual_sink = false;
    bool changed_default_sink = false;
    std::string previous_default_sink;   // the raw metadata value, JSON

    std::vector<float> live_levels() const {
        if (capture) return capture->levels();
        if (generator) return generator->levels();
        return {};
    }
};

Engine::Engine(Config config) : config_(std::move(config)) {}

Engine::~Engine() {
    stop();
    wait_until_stopped(std::chrono::seconds(10));
    if (worker_.joinable()) worker_.join();
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

EngineState Engine::state() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_.state;
}

std::vector<float> Engine::live_input_levels() const {
    std::shared_ptr<Pipeline> pipeline;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pipeline = pipeline_;
    }
    return pipeline ? pipeline->live_levels() : std::vector<float>{};
}

void Engine::set_state(EngineState state) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.state = state;
    }
    if (on_state) on_state(state);
}

void Engine::log(const std::string& message) {
    if (on_log) on_log(message);
}

void Engine::start() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (state_is_active(status_.state)) return;
        status_ = EngineStatus{};
        status_.state = EngineState::Starting;
        stop_requested_.store(false);
    }
    {
        std::lock_guard<std::mutex> guard(finished_mutex_);
        finished_ = false;
    }
    if (worker_.joinable()) worker_.join();
    if (on_state) on_state(EngineState::Starting);
    worker_ = std::thread([this] { run(); });
}

void Engine::stop() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!state_is_active(status_.state)) return;
    stop_requested_.store(true);
}

bool Engine::wait_until_stopped(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(finished_mutex_);
    return finished_cv_.wait_for(guard, timeout, [this] { return finished_; });
}

void Engine::run() {
    try {
        start_pipeline();
    } catch (const Failure& failure) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.failure = failure.what();
        }
        set_state(EngineState::Failed);
    } catch (const std::exception& error) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.failure = error.what();
        }
        set_state(EngineState::Failed);
    }
    std::lock_guard<std::mutex> guard(finished_mutex_);
    finished_ = true;
    finished_cv_.notify_all();
}

std::optional<NodeInfo> Engine::preview_output(const Config& config, const Snapshot& snap) {
    if (config.output_is_none() || config.output_is_alsa()) return std::nullopt;
    if (!config.output.empty()) {
        for (const NodeInfo* n : snap.sinks())
            if (n->name == config.output || n->base_name() == config.output || n->description == config.output)
                return *n;
        return std::nullopt;
    }
    auto ranked = ranked_bitstream_sinks(snap, config.codec, false);
    if (ranked.empty()) return std::nullopt;
    return ranked.front().node;
}

void Engine::start_pipeline() {
    config_.validate();
    auto pipeline = std::make_shared<Pipeline>();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pipeline_ = pipeline;
    }
    Pipeline& p = *pipeline;

    // Teardown runs whatever happens from here on, in reverse order, and
    // keeps any failure message alive across the Stopping transition.
    struct Teardown {
        Engine& engine;
        std::shared_ptr<Pipeline> pipeline;
        ~Teardown() {
            Pipeline& p = *pipeline;
            std::string failure;
            {
                std::lock_guard<std::mutex> guard(engine.mutex_);
                if (engine.status_.state == EngineState::Failed) failure = engine.status_.failure;
            }
            engine.set_state(EngineState::Stopping);
            auto attempt = [&](const char* what, auto&& fn) {
                try { fn(); } catch (const std::exception& e) { engine.log(fmt("while stopping {}: {}", what, e.what())); }
            };
            attempt("the output", [&] { if (p.sink) p.sink->stop(); });
            attempt("the capture", [&] { if (p.capture) p.capture->stop(); if (p.generator) p.generator->stop(); });
            attempt("the encoder", [&] { if (p.encoder) p.encoder->stop(); });
            attempt("restoring the default sink", [&] {
                if (p.changed_default_sink && p.session) {
                    if (p.previous_default_sink.empty()) p.session->clear_configured_default_sink();
                    else p.session->set_configured_default_sink(p.previous_default_sink);
                }
            });
            attempt("removing the virtual sink", [&] { if (p.created_virtual_sink && p.session) p.session->destroy_virtual_sink(); });
            p.sink.reset();
            p.capture.reset();
            p.generator.reset();
            p.encoder.reset();
            p.session.reset();
            {
                std::lock_guard<std::mutex> guard(engine.mutex_);
                engine.pipeline_.reset();
                if (!failure.empty()) engine.status_.failure = failure;
            }
            engine.set_state(failure.empty() ? EngineState::Idle : EngineState::Failed);
        }
    } teardown{*this, pipeline};

    const Config& c = config_;
    p.session = std::make_unique<PwSession>();
    Snapshot snap = p.session->snapshot();
    log(fmt("PipeWire {} on {}", snap.server_version, snap.server_name));

    // --- output ---------------------------------------------------------
    p.spdif_ring = std::make_unique<ByteRing>(static_cast<size_t>(c.burst_bytes()) * 64);
    p.pcm_ring = std::make_unique<ByteRing>(static_cast<size_t>(c.channels) * sizeof(float) * c.sample_rate);
    std::string output_description;
    if (c.output_is_none()) {
        p.sink = std::make_unique<NullSink>(*p.spdif_ring, c.sample_rate);
    } else if (c.output_is_alsa()) {
        std::string device = c.alsa_device();
        if (device.empty()) {
            auto pcms = alsa_digital_pcms();
            if (pcms.empty()) throw Failure("no iec958: or hdmi: ALSA playback device found");
            device = pcms.front().name;
            log(fmt("using ALSA device {} ({})", device, pcms.front().description));
        }
        p.sink = std::make_unique<AlsaSink>(c, *p.spdif_ring, device, [this](const std::string& m) { log(m); });
    } else {
        bool pcm_carrier = c.effective_transport() == Transport::Pcm;
        std::optional<NodeInfo> node;
        if (!c.output.empty()) {
            node = p.session->find_node(c.output, NodeRole::Sink);
            if (!node) throw Failure(fmt("no sink matches \"{}\" (see --list)", c.output));
        } else {
            auto ranked = ranked_bitstream_sinks(snap, c.codec, false);
            if (ranked.empty())
                throw Failure(fmt("no digital output sink found for {}. Name one with --output, "
                                  "or use --output alsa:<pcm> for the direct transport.", codec_short_name(c.codec)));
            node = ranked.front().node;
            if (ranked.size() > 1) {
                std::string others;
                for (size_t i = 1; i < ranked.size(); ++i) others += (i > 1 ? ", " : "") + ranked[i].node.display_name();
                log("also bitstream-capable: " + others);
            }
        }
        p.output_node = node;
        std::string port = port_type_of(snap, *node);
        if (!is_digital_port(port) && !port.empty())
            log(fmt("warning: {} reports a {} port; a bitstream into an analog output is full-scale noise",
                    node->display_name(), port));
        p.sink = std::make_unique<PwSink>(*p.session, c, *p.spdif_ring, *node, pcm_carrier,
                                          [this](const std::string& m) { log(m); });
    }

    // --- input ----------------------------------------------------------
    std::string input_name;
    if (c.self_test) {
        p.generator = std::make_unique<ToneGenerator>(c, *p.pcm_ring);
        input_name = "self-test generator";
    } else {
        std::optional<NodeInfo> target;
        bool target_is_sink = false;
        if (c.input == "auto" || c.input.empty()) {
            std::string name = c.virtual_sink_name;
            if (auto existing = p.session->find_node(name, NodeRole::Sink); existing && existing->name == name) {
                log(fmt("a sink named {} already exists; capturing from it", name));
                target = existing;
            } else {
                std::string description = fmt("AC3SPDIF {} encoder ({})", codec_short_name(c.codec), channel_layout_name(c.channels));
                target = p.session->create_virtual_sink(name, description, c.channels, c.sample_rate);
                p.created_virtual_sink = true;
                p.virtual_sink = target;
                log(fmt("created virtual sink \"{}\" ({} channels)", description, c.channels));
            }
            target_is_sink = true;
            if (c.set_default_sink) {
                p.previous_default_sink = p.session->configured_default_sink();
                p.session->set_configured_default_sink(target->name);
                p.changed_default_sink = true;
                log("system output now routed to the encoder; it is put back on stop");
            }
        } else {
            target = p.session->find_node(c.input, NodeRole::Any);
            if (!target) throw Failure(fmt("no sink or source matches \"{}\" (see --list)", c.input));
            target_is_sink = target->is_sink() && !target->is_source();
        }
        p.capture = std::make_unique<Capture>(*p.session, c, *p.pcm_ring, *target, target_is_sink);
        input_name = target->display_name() + (target_is_sink ? " (monitor)" : "");
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.input_name = input_name;
        status_.output_name = p.sink->device_name();
        status_.carrier = p.sink->carrier_label();
        status_.stream = p.sink->stream_label();
        status_.target_bursts = c.prebuffer_bursts;
    }

    if (c.bitrate_was_substituted())
        log(fmt("{} does not accept {}; using {} instead. Accepted: {}.", codec_name(c.codec), c.bitrate,
                c.effective_bitrate(), [&] { std::string s; for (auto& b : supported_bitrates(c.codec)) s += (s.empty() ? "" : ", ") + b; return s; }()));
    if (c.downmixes_to_surround51())
        log(fmt("capturing {} channels, but {} has no 7.1 mode: the side pair is folded into the "
                "back pair and 5.1 is sent", c.channels, codec_short_name(c.codec)));
    if (p.sink->is_pcm_carrier())
        log("PCM carrier: the IEC 60958 non-audio bit will NOT be set, so the receiver must detect "
            "the burst preamble itself. If this stream reaches an ANALOG output it will be full-scale noise.");

    // --- go -------------------------------------------------------------
    p.encoder = std::make_unique<Encoder>(c, *p.pcm_ring, *p.spdif_ring);
    p.encoder->on_log = [this](const std::string& m) { log(m); };
    p.encoder->start();
    if (p.capture) p.capture->start();
    if (p.generator) p.generator->start();

    // Claim the device and negotiate the format now, before the buffer target
    // matters. The encoder keeps producing throughout.
    p.sink->prepare();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        status_.format = p.sink->format_label();
    }
    int floor = latency_advice::minimum_bursts(p.sink->request_frames(), c.codec, c.sample_rate);
    if (c.prebuffer_bursts < floor)
        log(fmt("output buffer is {} burst(s); the floor for {} on this device is {}: expect dropouts",
                c.prebuffer_bursts, codec_name(c.codec), floor));

    // Fill the output ring before the device starts pulling, so the very
    // first burst the receiver sees is a complete one.
    const size_t target = static_cast<size_t>(c.prebuffer_bursts) * c.burst_bytes();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (p.spdif_ring->fill_level() < target && std::chrono::steady_clock::now() < deadline && !stop_requested()) {
        if (!p.encoder->running()) throw Failure("the encoder stopped during start-up: " + p.encoder->last_error());
        if (p.capture && p.capture->failed()) throw Failure("capture failed: " + p.capture->error());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stop_requested()) return;
    if (p.spdif_ring->fill_level() < target)
        throw Failure(fmt("timed out waiting for the first {} {} bursts; check that {} is producing audio",
                          c.prebuffer_bursts, codec_short_name(c.codec), input_name));

    // Whatever piled up beyond the target while the device was negotiating is
    // pure latency, and the drift controller would spend minutes trimming it
    // a sample at a time. Drop it in whole bursts, before anything has
    // reached the wire.
    size_t excess_bursts = (p.spdif_ring->fill_level() - target) / c.burst_bytes();
    if (excess_bursts > 0) p.spdif_ring->discard(excess_bursts * c.burst_bytes());

    p.sink->begin();
    if (auto* pw = dynamic_cast<PwSink*>(p.sink.get())) {
        std::string negotiated = pw->sink_format();
        std::lock_guard<std::mutex> guard(mutex_);
        status_.sink_format = negotiated;
        status_.format = p.sink->format_label();
    }
    set_state(EngineState::Running);

    while (!stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!p.encoder->running()) {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.failure = "the encoder stopped: " + p.encoder->last_error();
            status_.state = EngineState::Failed;
            return;
        }
        if (p.sink->failed() || (p.capture && p.capture->failed()) || p.session->failed()) {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.failure = p.sink->failed() ? "output stream failed: " + p.sink->error()
                            : p.capture && p.capture->failed() ? "capture stream failed: " + p.capture->error()
                            : p.session->error();
            status_.state = EngineState::Failed;
            return;
        }
        EncoderStats stats = p.encoder->stats();
        uint64_t input_samples = std::max<uint64_t>(stats.blocks_fed * c.frame_samples(), 1);
        int64_t device_delay = p.sink->device_delay_frames();
        int64_t capture_delay = p.capture ? p.capture->delay_frames() : 0;
        std::lock_guard<std::mutex> guard(mutex_);
        status_.seconds_on_wire = p.sink->rendered_bytes() / p.sink->bytes_per_second();
        status_.bursts = p.sink->rendered_bytes() / static_cast<uint64_t>(c.burst_bytes());
        status_.buffer_bursts = static_cast<int>(p.spdif_ring->fill_level() / c.burst_bytes());
        status_.drift_ppm = (static_cast<double>(stats.samples_dropped) - static_cast<double>(stats.samples_duplicated))
                            * 1e6 / static_cast<double>(input_samples);
        status_.underrun_bytes = p.sink->underrun_bytes();
        status_.capture_overflow_frames = p.capture ? p.capture->overflow_frames() : 0;
        status_.silence_blocks = stats.silence_blocks;
        status_.input_levels = p.live_levels();
        status_.input_peak_hold = p.capture ? p.capture->peak_hold() : p.live_levels();
        status_.source_channels = c.channels;
        LatencyBreakdown l;
        l.capture_frames = p.capture ? p.capture->quantum_frames() + static_cast<int>(capture_delay) : c.capture_buffer_frames;
        l.pcm_ring_frames = static_cast<int>(p.pcm_ring->fill_level() / (c.channels * sizeof(float)));
        l.encoder_frames = c.frame_samples();
        l.output_ring_frames = static_cast<int>(p.spdif_ring->fill_level() / kCarrierBytesPerFrame);
        l.device_frames = static_cast<int>(device_delay);
        status_.latency = l;
    }
}

}  // namespace ac3spdif
