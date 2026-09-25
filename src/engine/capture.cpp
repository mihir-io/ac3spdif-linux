#include "engine/capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include "engine/log.h"

namespace ac3spdif {

std::vector<uint32_t> spa_channel_ids(int channels) {
    std::vector<uint32_t> out;
    for (const auto& name : channel_position_names(channels)) {
        uint32_t id = SPA_AUDIO_CHANNEL_UNKNOWN;
        if (name == "MONO") id = SPA_AUDIO_CHANNEL_MONO;
        else if (name == "FL") id = SPA_AUDIO_CHANNEL_FL;
        else if (name == "FR") id = SPA_AUDIO_CHANNEL_FR;
        else if (name == "FC") id = SPA_AUDIO_CHANNEL_FC;
        else if (name == "LFE") id = SPA_AUDIO_CHANNEL_LFE;
        else if (name == "RL") id = SPA_AUDIO_CHANNEL_RL;
        else if (name == "RR") id = SPA_AUDIO_CHANNEL_RR;
        else if (name == "SL") id = SPA_AUDIO_CHANNEL_SL;
        else if (name == "SR") id = SPA_AUDIO_CHANNEL_SR;
        else if (name.rfind("AUX", 0) == 0) id = SPA_AUDIO_CHANNEL_AUX0 + std::atoi(name.c_str() + 3);
        out.push_back(id);
    }
    return out;
}

namespace {

void trampoline_process(void* data) { static_cast<Capture*>(data)->on_process(); }

void trampoline_state(void* data, enum pw_stream_state old, enum pw_stream_state state,
                      const char* error) {
    static_cast<Capture*>(data)->on_state_changed(old, state, error);
}

const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = trampoline_state,
    .process = trampoline_process,
};

}  // namespace

Capture::Capture(PwSession& session, const Config& config, ByteRing& ring, NodeInfo target,
                 bool target_is_sink)
    : session_(session), config_(config), ring_(ring), target_(std::move(target)),
      target_is_sink_(target_is_sink), channels_(config.channels) {
    for (auto& p : peak_) p.store(0.0f);
    for (auto& h : hold_) h.store(0.0f);
    listener_ = new spa_hook{};
}

Capture::~Capture() {
    stop();
    delete static_cast<spa_hook*>(listener_);
}

std::string Capture::error() const { return error_; }

void Capture::start() {
    auto guard = session_.lock();
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_NODE_NAME, "ac3spdif-capture",
        PW_KEY_NODE_DESCRIPTION, "AC3SPDIF encoder input",
        PW_KEY_APP_NAME, "ac3spdif",
        // If the target vanishes we want to know, not be moved to the
        // microphone.
        PW_KEY_NODE_DONT_RECONNECT, "true",
        nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", config_.capture_buffer_frames,
                       config_.sample_rate);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", config_.sample_rate);
    if (target_.serial) pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, target_.serial);
    else pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_.name.c_str());
    if (target_is_sink_) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    if (config_.link_group) pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, "ac3spdif");

    stream_ = pw_stream_new(session_.core(), "AC3SPDIF capture", props);
    if (!stream_) throw Failure("cannot create the capture stream");
    pw_stream_add_listener(stream_, static_cast<spa_hook*>(listener_), &stream_events, this);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
                                                      .rate = static_cast<uint32_t>(config_.sample_rate),
                                                      .channels = static_cast<uint32_t>(channels_));
    auto ids = spa_channel_ids(channels_);
    for (size_t i = 0; i < ids.size() && i < SPA_AUDIO_MAX_CHANNELS; ++i) info.position[i] = ids[i];
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
                                                             | PW_STREAM_FLAG_MAP_BUFFERS
                                                             | PW_STREAM_FLAG_RT_PROCESS),
                                params, 1);
    if (res < 0) throw Failure(fmt("cannot connect the capture stream: {}", spa_strerror(res)));

    for (int waited = 0;; ++waited) {
        const char* err = nullptr;
        enum pw_stream_state state = pw_stream_get_state(stream_, &err);
        if (state == PW_STREAM_STATE_STREAMING) break;
        if (state == PW_STREAM_STATE_ERROR)
            throw Failure(fmt("capture stream failed: {}", err ? err : "unknown error"));
        if (waited >= 8)
            throw Failure(fmt("the capture stream never started on {}; is anything driving it?",
                              target_.display_name()));
        pw_thread_loop_timed_wait(session_.loop(), 1);
    }
}

void Capture::stop() {
    if (!stream_) return;
    auto guard = session_.lock();
    spa_hook_remove(static_cast<spa_hook*>(listener_));
    pw_stream_destroy(stream_);
    stream_ = nullptr;
}

int64_t Capture::delay_frames() {
    if (!stream_) return 0;
    auto guard = session_.lock();
    pw_time t{};
    if (pw_stream_get_time_n(stream_, &t, sizeof(t)) < 0) return 0;
    if (t.rate.denom == 0) return 0;
    // delay is in units of t.rate; normalise to frames at the capture rate.
    double seconds = static_cast<double>(t.delay) * t.rate.num / t.rate.denom;
    return static_cast<int64_t>(seconds * config_.sample_rate) + static_cast<int64_t>(t.queued);
}

std::vector<float> Capture::levels() const {
    std::vector<float> out;
    for (int c = 0; c < channels_ && c < kMaxMeteredChannels; ++c) out.push_back(peak_[c].load(std::memory_order_relaxed));
    return out;
}

std::vector<float> Capture::peak_hold() const {
    std::vector<float> out;
    for (int c = 0; c < channels_ && c < kMaxMeteredChannels; ++c) out.push_back(hold_[c].load(std::memory_order_relaxed));
    return out;
}

void Capture::on_state_changed(int, int state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        error_ = error ? error : "stream error";
        failed_.store(true);
    }
    session_.signal();
}

// Realtime path.
void Capture::on_process() {
    pw_buffer* b;
    while ((b = pw_stream_dequeue_buffer(stream_)) != nullptr) {
        spa_buffer* buf = b->buffer;
        spa_data& d = buf->datas[0];
        if (d.data && d.chunk) {
            uint32_t stride = d.chunk->stride ? d.chunk->stride : static_cast<uint32_t>(channels_ * sizeof(float));
            uint32_t size = d.chunk->size;
            const uint8_t* src = static_cast<const uint8_t*>(d.data) + d.chunk->offset;
            uint32_t frames = stride ? size / stride : 0;
            if (frames > 0) {
                quantum_frames_.store(static_cast<int>(frames), std::memory_order_relaxed);
                // Meters: a decay of about 350 ms to silence, applied per
                // block so it looks the same at any quantum.
                float decay = std::exp(-static_cast<float>(frames) / (0.35f * config_.sample_rate));
                const float* samples = reinterpret_cast<const float*>(src);
                int metered = channels_ < kMaxMeteredChannels ? channels_ : kMaxMeteredChannels;
                for (int c = 0; c < metered; ++c) {
                    float peak = 0.0f;
                    for (uint32_t f = 0; f < frames; ++f) {
                        float v = std::fabs(samples[f * channels_ + c]);
                        if (v > peak) peak = v;
                    }
                    if (peak > 1.0f) peak = 1.0f;
                    float current = peak_[c].load(std::memory_order_relaxed) * decay;
                    peak_[c].store(peak > current ? peak : current, std::memory_order_relaxed);
                    if (peak > hold_[c].load(std::memory_order_relaxed))
                        hold_[c].store(peak, std::memory_order_relaxed);
                }
                size_t written = ring_.write(src, size);
                if (written < size)
                    overflow_frames_.fetch_add((size - written) / stride, std::memory_order_relaxed);
                delivered_frames_.fetch_add(frames, std::memory_order_relaxed);
            }
        }
        pw_stream_queue_buffer(stream_, b);
    }
}

}  // namespace ac3spdif
