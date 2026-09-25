#include "engine/pw_sink.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/iec958-types.h>
#include <spa/param/audio/type-info.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>

#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace ac3spdif {

namespace {

void trampoline_process(void* data) { static_cast<PwSink*>(data)->on_process(); }
void trampoline_state(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    static_cast<PwSink*>(data)->on_state_changed(old, state, error);
}
void trampoline_param(void* data, uint32_t id, const spa_pod* param) {
    static_cast<PwSink*>(data)->on_param_changed(id, param);
}

const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = trampoline_state,
    .param_changed = trampoline_param,
    .process = trampoline_process,
};

uint32_t spa_codec(Codec c) {
    return c == Codec::AC3 ? SPA_AUDIO_IEC958_CODEC_AC3 : SPA_AUDIO_IEC958_CODEC_DTS;
}

}  // namespace

PwSink::PwSink(PwSession& session, const Config& config, ByteRing& ring, NodeInfo sink,
               bool pcm_carrier, LogSink log)
    : session_(session), config_(config), ring_(ring), sink_(std::move(sink)),
      pcm_carrier_(pcm_carrier), log_(std::move(log)) {
    listener_ = new spa_hook{};
}

PwSink::~PwSink() {
    stop();
    delete static_cast<spa_hook*>(listener_);
}

void PwSink::prepare() {
    if (!pcm_carrier_) {
        std::string codec_name = codec_pipewire_name(config_.codec);
        if (!sink_.has_codec(config_.codec)) {
            if (log_) log_(fmt("{} does not advertise {} yet; enabling it on the sink",
                               sink_.display_name(), codec_name));
            if (!session_.enable_iec958_codecs(sink_, {codec_name}))
                throw Failure(fmt("{} would not accept {} in its IEC 958 codec list. Choose a "
                                  "different output, or use the PCM carrier or direct ALSA.",
                                  sink_.display_name(), codec_name));
            if (auto refreshed = session_.node_by_id(sink_.id)) sink_ = *refreshed;
        }
    } else {
        // An encoded passthrough stream is non-mixable and nothing scales it.
        // A plain stereo stream gets no such protection: a sink volume below
        // unity multiplies every sample and turns the burst into noise.
        saved_levels_ = session_.neutralise_sink_levels(sink_);
    }

    auto guard = session_.lock();
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "ac3spdif-bitstream",
        PW_KEY_NODE_DESCRIPTION, pcm_carrier_ ? "AC3SPDIF bitstream (PCM carrier)" : "AC3SPDIF bitstream",
        PW_KEY_APP_NAME, "ac3spdif",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", config_.device_buffer_frames, config_.sample_rate);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", config_.sample_rate);
    if (sink_.serial) pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, sink_.serial);
    else pw_properties_set(props, PW_KEY_TARGET_OBJECT, sink_.name.c_str());
    if (config_.link_group) pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, "ac3spdif");
    if (pcm_carrier_) {
        // No remixing, no channel games: two channels in, the same two out.
        pw_properties_set(props, PW_KEY_STREAM_DONT_REMIX, "true");
        pw_properties_set(props, "channelmix.disable", "true");
        pw_properties_set(props, "resample.disable", "true");
    }

    stream_ = pw_stream_new(session_.core(), "AC3SPDIF bitstream", props);
    if (!stream_) throw Failure("cannot create the bitstream stream");
    pw_stream_add_listener(stream_, static_cast<spa_hook*>(listener_), &stream_events, this);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1];
    if (pcm_carrier_) {
        spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16_LE,
                                                          .rate = static_cast<uint32_t>(config_.sample_rate),
                                                          .channels = 2);
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
    } else {
        spa_audio_info_iec958 info = SPA_AUDIO_INFO_IEC958_INIT(
            .codec = static_cast<enum spa_audio_iec958_codec>(spa_codec(config_.codec)),
            .rate = static_cast<uint32_t>(config_.sample_rate));
        params[0] = spa_format_audio_iec958_build(&b, SPA_PARAM_EnumFormat, &info);
    }

    // Connected inactive: the link and the format negotiation happen now, the
    // first byte leaves only when begin() says so.
    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
                     | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE;
    if (!pcm_carrier_) flags |= PW_STREAM_FLAG_EXCLUSIVE;
    int res = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                static_cast<pw_stream_flags>(flags), params, 1);
    if (res < 0) throw Failure(fmt("cannot connect the bitstream stream: {}", spa_strerror(res)));
    wait_for_state(PW_STREAM_STATE_PAUSED, "link the bitstream to the sink");
}

void PwSink::wait_for_state(int wanted, const char* what) {
    for (int waited = 0;; ++waited) {
        const char* err = nullptr;
        enum pw_stream_state state = pw_stream_get_state(stream_, &err);
        if (static_cast<int>(state) == wanted || state == PW_STREAM_STATE_STREAMING) return;
        if (state == PW_STREAM_STATE_ERROR)
            throw Failure(fmt("cannot {}: {}", what, err ? err : "stream error"));
        if (waited >= 8)
            throw Failure(fmt("timed out trying to {} on {}. If the sink is in use by another "
                              "exclusive stream, stop that first.", what, sink_.display_name()));
        pw_thread_loop_timed_wait(session_.loop(), 1);
    }
}

void PwSink::begin() {
    auto guard = session_.lock();
    pw_stream_set_active(stream_, true);
    wait_for_state(PW_STREAM_STATE_STREAMING, "start the bitstream");
}

void PwSink::stop() {
    if (stream_) {
        auto guard = session_.lock();
        spa_hook_remove(static_cast<spa_hook*>(listener_));
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (saved_levels_) {
        try { session_.restore_sink_levels(sink_, *saved_levels_); } catch (...) {}
        saved_levels_.reset();
    }
}

int64_t PwSink::device_delay_frames() {
    if (!stream_) return 0;
    auto guard = session_.lock();
    pw_time t{};
    if (pw_stream_get_time_n(stream_, &t, sizeof(t)) < 0 || t.rate.denom == 0) return 0;
    double seconds = static_cast<double>(t.delay) * t.rate.num / t.rate.denom;
    return static_cast<int64_t>(seconds * config_.sample_rate) + static_cast<int64_t>(t.queued);
}

std::string PwSink::sink_format() { return session_.current_format(sink_.id); }

// The stream's own Format event can lag the link; until it arrives, describe
// what was asked for.
std::string PwSink::format_label() const {
    if (!format_label_.empty()) return format_label_;
    if (pcm_carrier_) return fmt("{} Hz S16LE 2ch (requested)", config_.sample_rate);
    return fmt("{} Hz iec958 {} 2ch 16-bit (requested)", config_.sample_rate, codec_pipewire_name(config_.codec));
}

void PwSink::on_state_changed(int, int state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        error_ = error ? error : "stream error";
        failed_.store(true);
    }
    session_.signal();
}

void PwSink::on_param_changed(uint32_t id, const spa_pod* param) {
    if (id != SPA_PARAM_Format || !param) return;
    uint32_t media_type = 0, media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return;
    if (media_subtype == SPA_MEDIA_SUBTYPE_iec958) {
        spa_audio_info_iec958 info{};
        if (spa_format_audio_iec958_parse(param, &info) == 0) {
            const char* name = spa_debug_type_find_short_name(spa_type_audio_iec958_codec, info.codec);
            format_label_ = fmt("{} Hz iec958 {} 2ch 16-bit", info.rate, name ? name : "?");
        }
    } else if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) == 0) {
            const char* name = spa_debug_type_find_short_name(spa_type_audio_format, info.format);
            format_label_ = fmt("{} Hz {} {}ch", info.rate, name ? name : "?", info.channels);
        }
    }
}

// Realtime path. Copy bytes straight from the ring into the buffer. No
// interpretation: every byte of the IEC 61937 stream must reach the wire in
// order and exactly once, or the receiver loses burst alignment.
void PwSink::on_process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b) return;
    spa_buffer* buf = b->buffer;
    spa_data& d = buf->datas[0];
    if (d.data && d.chunk) {
        const uint32_t stride = kCarrierBytesPerFrame;
        uint32_t frames = d.maxsize / stride;
        if (b->requested && b->requested < frames) frames = static_cast<uint32_t>(b->requested);
        uint32_t bytes = frames * stride;
        size_t shortfall = ring_.read_zero_padded(d.data, bytes);
        d.chunk->offset = 0;
        d.chunk->stride = static_cast<int32_t>(stride);
        d.chunk->size = bytes;
        b->size = frames;
        request_frames_.store(static_cast<int>(frames), std::memory_order_relaxed);
        rendered_.fetch_add(bytes, std::memory_order_relaxed);
        underrun_.fetch_add(shortfall, std::memory_order_relaxed);
    }
    pw_stream_queue_buffer(stream_, b);
}

}  // namespace ac3spdif
