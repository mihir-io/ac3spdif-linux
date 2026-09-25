#include "engine/alsa_sink.h"

#include <alsa/asoundlib.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "engine/alsa_devices.h"
#include "engine/reserve.h"

namespace ac3spdif {

AlsaSink::AlsaSink(const Config& config, ByteRing& ring, std::string pcm_name, LogSink log)
    : config_(config), ring_(ring), pcm_name_(std::move(pcm_name)), log_(std::move(log)) {
    full_name_ = alsa_iec958_name(pcm_name_, config_.sample_rate);
    description_ = pcm_name_;
    for (const auto& pcm : alsa_digital_pcms())
        if (pcm.name == pcm_name_) description_ = pcm.description + " (" + pcm_name_ + ")";
}

AlsaSink::~AlsaSink() { stop(); }

void AlsaSink::prepare() {
    int card = alsa_card_index(pcm_name_);
    if (card >= 0) {
        reservation_ = std::make_unique<DeviceReservation>(card, "ac3spdif", 0);
        try {
            std::string note = reservation_->acquire();
            if (log_) log_(fmt("reserved ALSA card {}: {}", card, note));
        } catch (const Failure& failure) {
            if (log_) log_(fmt("could not reserve card {} over D-Bus ({}); trying to open it anyway",
                               card, failure.what()));
            reservation_.reset();
        }
    }
    if (full_name_ == pcm_name_ && log_)
        log_("this PCM name takes no AES arguments, so the IEC 958 non-audio bit will not be set; "
             "prefer an iec958: or hdmi: name");

    // The sound server closes the device asynchronously after releasing the
    // reservation; try for a couple of seconds before giving up.
    int err = -EBUSY;
    for (int attempt = 0; attempt < 40 && err == -EBUSY; ++attempt) {
        err = snd_pcm_open(&pcm_, full_name_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err == -EBUSY) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (err < 0)
        throw Failure(fmt("cannot open ALSA device {}: {}{}", full_name_, snd_strerror(err),
                          err == -EBUSY ? " (still held by the sound server or another program)" : ""));

    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_, hw);
    if ((err = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
        || (err = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0
        || (err = snd_pcm_hw_params_set_channels(pcm_, hw, 2)) < 0
        || (err = snd_pcm_hw_params_set_rate(pcm_, hw, static_cast<unsigned>(config_.sample_rate), 0)) < 0)
        throw Failure(fmt("{} cannot do 16-bit stereo at {} Hz: {}", pcm_name_, config_.sample_rate,
                          snd_strerror(err)));
    snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(std::max(64, config_.device_buffer_frames / 4));
    snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(config_.device_buffer_frames);
    int dir = 0;
    snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, &dir);
    snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer);
    if ((err = snd_pcm_hw_params(pcm_, hw)) < 0)
        throw Failure(fmt("cannot configure {}: {}", pcm_name_, snd_strerror(err)));
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    period_frames_ = static_cast<int>(period);
    buffer_frames_ = static_cast<int>(buffer);

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_, sw);
    // Start once the whole buffer is primed, so the first thing on the wire
    // is a continuous stream rather than a stutter.
    snd_pcm_sw_params_set_start_threshold(pcm_, sw, buffer);
    snd_pcm_sw_params_set_avail_min(pcm_, sw, period);
    snd_pcm_sw_params(pcm_, sw);

    format_label_ = fmt("{} Hz S16_LE 2ch, period {} frames, buffer {} frames", config_.sample_rate,
                        period_frames_, buffer_frames_);
    int card_for_status = alsa_card_index(pcm_name_);
    std::string status = alsa_iec958_status(card_for_status);
    if (!status.empty() && log_) log_("IEC 958 channel status now " + status);
}

void AlsaSink::begin() {
    if (!pcm_) throw Failure("ALSA device was not prepared");
    int err = snd_pcm_prepare(pcm_);
    if (err < 0) throw Failure(fmt("cannot prepare {}: {}", pcm_name_, snd_strerror(err)));
    running_.store(true);
    thread_ = std::thread([this] { writer_loop(); });
}

void AlsaSink::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    if (reservation_) {
        try { reservation_->release(); } catch (...) {}
        reservation_.reset();
    }
}

// Blocking writes of one period at a time: the kernel paces this thread, and
// the ring upstream absorbs the encoder's burst-sized deliveries.
void AlsaSink::writer_loop() {
    const size_t period_bytes = static_cast<size_t>(period_frames_) * kCarrierBytesPerFrame;
    std::vector<uint8_t> chunk(period_bytes);
    while (running_.load()) {
        size_t shortfall = ring_.read_zero_padded(chunk.data(), period_bytes);
        snd_pcm_sframes_t written = snd_pcm_writei(pcm_, chunk.data(), static_cast<snd_pcm_uframes_t>(period_frames_));
        if (written == -EPIPE) {
            xruns_.fetch_add(1);
            underrun_.fetch_add(period_bytes);
            snd_pcm_prepare(pcm_);
            continue;
        }
        if (written < 0) {
            written = snd_pcm_recover(pcm_, static_cast<int>(written), 1);
            if (written < 0) {
                error_ = fmt("ALSA write failed: {}", snd_strerror(static_cast<int>(written)));
                failed_.store(true);
                return;
            }
            continue;
        }
        rendered_.fetch_add(static_cast<uint64_t>(written) * kCarrierBytesPerFrame);
        underrun_.fetch_add(shortfall);
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm_, &delay) == 0) delay_frames_.store(delay);
    }
}

}  // namespace ac3spdif
