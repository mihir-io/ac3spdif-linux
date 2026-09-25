// The direct ALSA sink: hog mode, Linux style.
//
// Reserve the card over D-Bus so the sound server closes it, open the IEC 958
// PCM with the channel-status bits that mark the stream non-audio, and write
// bursts straight into the kernel's ring. Nothing else touches the samples.
// Everything is handed back on stop: the device closes and the reservation is
// released, at which point WirePlumber reopens the card.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "engine/config.h"
#include "engine/log.h"
#include "engine/ring.h"
#include "engine/sink.h"

typedef struct _snd_pcm snd_pcm_t;

namespace ac3spdif {

class DeviceReservation;

class AlsaSink : public BitstreamSink {
public:
    AlsaSink(const Config& config, ByteRing& ring, std::string pcm_name, LogSink log);
    ~AlsaSink() override;

    void prepare() override;
    void begin() override;
    void stop() override;

    uint64_t rendered_bytes() const override { return rendered_.load(std::memory_order_relaxed); }
    uint64_t underrun_bytes() const override { return underrun_.load(std::memory_order_relaxed); }
    double bytes_per_second() const override { return config_.sample_rate * 4.0; }
    std::string device_name() const override { return description_; }
    std::string carrier_label() const override { return "alsa"; }
    std::string format_label() const override { return format_label_; }
    std::string stream_label() const override { return full_name_; }
    bool is_pcm_carrier() const override { return false; }
    int request_frames() const override { return period_frames_; }
    int64_t device_delay_frames() override { return delay_frames_.load(std::memory_order_relaxed); }
    bool failed() const override { return failed_.load(); }
    std::string error() const override { return error_; }
    uint64_t xruns() const { return xruns_.load(); }

private:
    void writer_loop();

    Config config_;
    ByteRing& ring_;
    std::string pcm_name_, full_name_, description_, format_label_;
    LogSink log_;
    std::unique_ptr<DeviceReservation> reservation_;
    snd_pcm_t* pcm_ = nullptr;
    int period_frames_ = 0;
    int buffer_frames_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> rendered_{0};
    std::atomic<uint64_t> underrun_{0};
    std::atomic<uint64_t> xruns_{0};
    std::atomic<int64_t> delay_frames_{0};
    std::atomic<bool> failed_{false};
    std::string error_;
};

}  // namespace ac3spdif
