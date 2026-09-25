#include "engine/encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ac3spdif {

namespace {

std::string av_error_string(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

constexpr int kAvioBufferSize = 16384;   // more than one burst, so each arrives whole

}  // namespace

Encoder::Encoder(const Config& config, ByteRing& pcm_ring, ByteRing& spdif_ring)
    : config_(config), pcm_ring_(pcm_ring), spdif_ring_(spdif_ring),
      block_frames_(config.frame_samples()), burst_bytes_(config.burst_bytes()),
      target_fill_(static_cast<long>(config.prebuffer_bursts) * config.burst_bytes()),
      deadband_(config.burst_bytes()) {}

Encoder::~Encoder() { stop(); }

std::string Encoder::last_error() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return last_error_;
}

EncoderStats Encoder::stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
}

void Encoder::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        last_error_ = message;
    }
    running_.store(false);
    if (on_log) on_log("encoder: " + message);
}

int Encoder::write_trampoline(void* opaque, const uint8_t* data, int size) {
    return static_cast<Encoder*>(opaque)->on_write(data, size);
}

// libavformat hands over each muxed burst here, from the feed thread.
int Encoder::on_write(const uint8_t* data, int size) {
    int pushed = 0;
    while (pushed < size) {
        size_t got = spdif_ring_.write(data + pushed, static_cast<size_t>(size - pushed));
        if (got == 0) {
            // Ring full: the output device has not consumed yet. Sleeping here
            // back-pressures the encoder, which is intended, but never wedge a
            // stop.
            if (!running_.load()) return size;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            pushed += static_cast<int>(got);
        }
    }
    if (dump_fd_ >= 0) {
        int saved = 0;
        while (saved < size) {
            ssize_t w = ::write(dump_fd_, data + saved, static_cast<size_t>(size - saved));
            if (w <= 0) break;
            saved += static_cast<int>(w);
        }
    }
    std::lock_guard<std::mutex> guard(mutex_);
    stats_.burst_bytes += static_cast<uint64_t>(size);
    return size;
}

void Encoder::open_codec() {
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_encoder_name(config_.codec));
    if (!codec)
        throw Failure(fmt("this libavcodec has no '{}' encoder; install a full ffmpeg build",
                          codec_encoder_name(config_.codec)));
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) throw Failure("cannot allocate the encoder context");
    codec_ctx_->sample_rate = config_.sample_rate;
    codec_ctx_->bit_rate = bitrate_bps(config_.effective_bitrate());
    codec_ctx_->time_base = AVRational{1, config_.sample_rate};
    // Known per encoder rather than queried, so this builds against every
    // libavcodec from 6 to 8 without touching the deprecated capability API.
    codec_ctx_->sample_fmt = config_.codec == Codec::AC3 ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S32;
    // The output layout is always stated, never inferred. With an 8-channel
    // capture the resampler would otherwise pick something on its own.
    AVChannelLayout out_layout{};
    std::string layout_name = config_.encode_layout();
    if (av_channel_layout_from_string(&out_layout, layout_name.c_str()) < 0)
        throw Failure("unknown encoder channel layout: " + layout_name);
    av_channel_layout_copy(&codec_ctx_->ch_layout, &out_layout);
    // ffmpeg's DTS encoder is marked experimental and refuses to run without
    // being told that is acceptable.
    if (config_.codec == Codec::DTS) codec_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    int res = avcodec_open2(codec_ctx_, codec, nullptr);
    if (res < 0)
        throw Failure(fmt("cannot open the {} encoder at {}: {}", codec_short_name(config_.codec),
                          config_.effective_bitrate(), av_error_string(res)));
    if (codec_ctx_->frame_size != block_frames_)
        throw Failure(fmt("encoder frame size is {} samples, expected {}", codec_ctx_->frame_size,
                          block_frames_));

    // Capture layout -> encoder layout and sample format.
    AVChannelLayout in_layout{};
    std::string capture_layout = config_.channel_layout();
    if (av_channel_layout_from_string(&in_layout, capture_layout.c_str()) < 0)
        throw Failure("unknown capture channel layout: " + capture_layout);
    if (in_layout.nb_channels != config_.channels)
        throw Failure(fmt("layout {} has {} channels but {} are captured", capture_layout,
                          in_layout.nb_channels, config_.channels));
    res = swr_alloc_set_opts2(&swr_, &out_layout, codec_ctx_->sample_fmt, config_.sample_rate,
                              &in_layout, AV_SAMPLE_FMT_FLT, config_.sample_rate, 0, nullptr);
    if (res < 0 || swr_init(swr_) < 0) throw Failure("cannot set up the sample converter");
    av_channel_layout_uninit(&in_layout);

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) throw Failure("cannot allocate encoder buffers");
    frame_->nb_samples = block_frames_;
    frame_->format = codec_ctx_->sample_fmt;
    frame_->sample_rate = config_.sample_rate;
    av_channel_layout_copy(&frame_->ch_layout, &out_layout);
    av_channel_layout_uninit(&out_layout);
    if (av_frame_get_buffer(frame_, 0) < 0) throw Failure("cannot allocate the encoder frame");

    // The IEC 61937 framing.
    res = avformat_alloc_output_context2(&format_ctx_, nullptr, "spdif", nullptr);
    if (res < 0 || !format_ctx_)
        throw Failure("this libavformat has no 'spdif' muxer; install a full ffmpeg build");
    auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    avio_ = avio_alloc_context(buffer, kAvioBufferSize, 1, this, nullptr, write_trampoline, nullptr);
    if (!avio_) { av_free(buffer); throw Failure("cannot allocate the muxer I/O context"); }
    format_ctx_->pb = avio_;
    format_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
    AVStream* stream = avformat_new_stream(format_ctx_, nullptr);
    if (!stream) throw Failure("cannot create the muxer stream");
    avcodec_parameters_from_context(stream->codecpar, codec_ctx_);
    stream->time_base = AVRational{1, config_.sample_rate};
    res = avformat_write_header(format_ctx_, nullptr);
    if (res < 0) throw Failure("cannot start the IEC 61937 muxer: " + av_error_string(res));
}

void Encoder::close_codec() {
    if (format_ctx_) {
        if (format_ctx_->pb) av_write_trailer(format_ctx_);
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }
    if (avio_) {
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    if (swr_) swr_free(&swr_);
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
}

void Encoder::start() {
    if (!config_.dump_path.empty()) {
        dump_fd_ = ::open(config_.dump_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dump_fd_ < 0)
            throw Failure(fmt("cannot open dump file {}: {}", config_.dump_path, std::strerror(errno)));
    }
    try {
        open_codec();
    } catch (...) {
        close_codec();
        throw;
    }
    running_.store(true);
    thread_ = std::thread([this] { feed_loop(); });
}

void Encoder::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    close_codec();
    if (dump_fd_ >= 0) {
        ::close(dump_fd_);
        dump_fd_ = -1;
    }
}

// The ceiling is scaled by block length. `max_drift_frames` is a count of
// samples per block, but a block is a whole encoded frame, 1536 samples for
// AC-3 and 512 for DTS, so the same count is three times the correction *rate*
// on DTS. Scaling keeps the ppm ceiling the same for every codec.
int Encoder::max_correction_frames() const {
    return std::max(1, config_.max_drift_frames * block_frames_ / 1536);
}

int Encoder::correction_frames(long error_bytes) const {
    long bursts_off = error_bytes / burst_bytes_;
    return static_cast<int>(std::min<long>(std::max<long>(bursts_off, 1), max_correction_frames()));
}

// Wait for `byte_count` more bytes of PCM, giving up after ~120 ms so a
// stalled capture cannot wedge the pipeline. Returns how many bytes were
// appended at `offset`.
size_t Encoder::read_blocking(uint8_t* destination, size_t offset, size_t byte_count) {
    size_t filled = 0;
    int idle = 0;
    while (running_.load() && filled < byte_count) {
        size_t got = pcm_ring_.read(destination + offset + filled, byte_count - filled);
        if (got == 0) {
            if (++idle > 120) return filled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            idle = 0;
            filled += got;
        }
    }
    return filled;
}

bool Encoder::encode_block(const float* interleaved) {
    if (av_frame_make_writable(frame_) < 0) { fail("frame not writable"); return false; }
    const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(interleaved)};
    int converted = swr_convert(swr_, frame_->data, block_frames_, in, block_frames_);
    if (converted != block_frames_) { fail("sample conversion produced a short block"); return false; }
    frame_->pts = next_pts_;
    next_pts_ += block_frames_;
    int res = avcodec_send_frame(codec_ctx_, frame_);
    if (res < 0) { fail("encoder rejected a frame: " + av_error_string(res)); return false; }
    while ((res = avcodec_receive_packet(codec_ctx_, packet_)) == 0) {
        packet_->stream_index = 0;
        int written = av_write_frame(format_ctx_, packet_);
        av_packet_unref(packet_);
        if (written < 0) { fail("muxer rejected a packet: " + av_error_string(written)); return false; }
    }
    if (res != AVERROR(EAGAIN) && res != AVERROR_EOF) {
        fail("encoder error: " + av_error_string(res));
        return false;
    }
    return true;
}

void Encoder::feed_loop() {
    const int channels = config_.channels;
    const size_t frame_bytes = static_cast<size_t>(channels) * sizeof(float);
    const int max_frames = block_frames_ + max_correction_frames();
    std::vector<float> scratch(static_cast<size_t>(max_frames) * channels);
    std::vector<float> silence(static_cast<size_t>(block_frames_) * channels, 0.0f);

    // A block that could not be completed before the read timeout is kept in
    // `scratch` and finished on a later pass. Throwing the partial data away
    // instead would advance the ring by a fraction of a frame and rotate the
    // channel interleave for the rest of the session.
    size_t pending_bytes = 0;
    int frames_to_consume = block_frames_;
    int dropped = 0, duplicated = 0;

    while (running_.load()) {
        if (pending_bytes == 0) {
            // Proportional, not bang-bang. A fixed one-sample nudge is ample
            // for the mismatch between two good crystals, but a bad clock can
            // saturate it, and a transient takes minutes to work off at one
            // sample per block. Scaling the correction with the error gives
            // headroom and still settles to a one-sample trim near target.
            long error = static_cast<long>(spdif_ring_.fill_level()) - target_fill_;
            frames_to_consume = block_frames_;
            dropped = duplicated = 0;
            if (error > deadband_) {
                dropped = correction_frames(error);        // capture clock fast: shed
                frames_to_consume += dropped;
            } else if (error < -deadband_) {
                duplicated = correction_frames(-error);    // capture clock slow: pad
                frames_to_consume -= duplicated;
            }
        }
        size_t wanted = static_cast<size_t>(frames_to_consume) * frame_bytes;
        pending_bytes += read_blocking(reinterpret_cast<uint8_t*>(scratch.data()), pending_bytes,
                                       wanted - pending_bytes);
        if (!running_.load()) break;
        if (pending_bytes < wanted) {
            // Capture stalled, usually because nothing is driving the source.
            // Emit a block of digital silence so the receiver holds its lock,
            // and leave the partial block in place for the next pass.
            if (!encode_block(silence.data())) return;
            std::lock_guard<std::mutex> guard(mutex_);
            stats_.silence_blocks++;
            continue;
        }
        pending_bytes = 0;

        if (duplicated > 0) {
            // Repeat the final frame to bring the block back up to length.
            const float* last = scratch.data() + static_cast<size_t>(frames_to_consume - 1) * channels;
            for (int extra = 0; extra < duplicated; ++extra) {
                float* target = scratch.data() + static_cast<size_t>(frames_to_consume + extra) * channels;
                std::copy(last, last + channels, target);
            }
        }
        // When shedding, the surplus tail frames are simply not encoded.
        if (!encode_block(scratch.data())) return;

        std::lock_guard<std::mutex> guard(mutex_);
        stats_.blocks_fed++;
        stats_.samples_dropped += static_cast<uint64_t>(dropped);
        stats_.samples_duplicated += static_cast<uint64_t>(duplicated);
    }
}

}  // namespace ac3spdif
