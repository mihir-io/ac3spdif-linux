#include "engine/selftest.h"

#include <chrono>
#include <cmath>

namespace ac3spdif {

namespace {
// One steady tone per channel, each a different pitch, so a listener can
// confirm not just that Dolby decoding engaged but that the channels arrived
// in the right order. The LFE gets 82 Hz, which a subwoofer can reproduce.
constexpr double kPitches[8] = {440, 554, 659, 82, 880, 1046, 1318, 1568};
constexpr float kAmplitude = 0.15f;
}  // namespace

ToneGenerator::ToneGenerator(const Config& config, ByteRing& ring)
    : config_(config), ring_(ring), block_frames_(config.capture_buffer_frames) {}

ToneGenerator::~ToneGenerator() { stop(); }

void ToneGenerator::start() {
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void ToneGenerator::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

std::vector<float> ToneGenerator::levels() const {
    return std::vector<float>(static_cast<size_t>(config_.channels), kAmplitude);
}

void ToneGenerator::run() {
    const int channels = config_.channels;
    const double rate = config_.sample_rate;
    std::vector<float> block(static_cast<size_t>(block_frames_ * channels));
    std::vector<double> phase(static_cast<size_t>(channels), 0.0);
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds(static_cast<int64_t>(1e9 * block_frames_ / rate));
    while (running_.load()) {
        for (int f = 0; f < block_frames_; ++f) {
            for (int c = 0; c < channels; ++c) {
                block[static_cast<size_t>(f * channels + c)] = kAmplitude * static_cast<float>(std::sin(phase[c]));
                phase[c] += 2.0 * M_PI * kPitches[c % 8] / rate;
                if (phase[c] > 2.0 * M_PI) phase[c] -= 2.0 * M_PI;
            }
        }
        ring_.write(block.data(), block.size() * sizeof(float));
        delivered_frames_.fetch_add(block_frames_);
        next += period;
        std::this_thread::sleep_until(next);
    }
}

}  // namespace ac3spdif
