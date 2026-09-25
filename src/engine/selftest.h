// Self-test source: a distinct tone per channel, paced at wall-clock rate the
// way a capture device would be, written into the PCM ring in place of the
// capture. Exercises the whole output path, including the receiver handshake
// and the channel order, without any system audio being involved.
#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "engine/config.h"
#include "engine/ring.h"

namespace ac3spdif {

class ToneGenerator {
public:
    ToneGenerator(const Config& config, ByteRing& ring);
    ~ToneGenerator();
    void start();
    void stop();
    std::vector<float> levels() const;
    uint64_t delivered_frames() const { return delivered_frames_.load(); }
    int block_frames() const { return block_frames_; }

private:
    void run();
    Config config_;
    ByteRing& ring_;
    int block_frames_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> delivered_frames_{0};
};

}  // namespace ac3spdif
