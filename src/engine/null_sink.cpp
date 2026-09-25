#include "engine/null_sink.h"

#include <chrono>

namespace ac3spdif {

void NullSink::begin() {
    running_.store(true);
    thread_ = std::thread([this] {
        const double interval = 0.05;
        const size_t chunk = static_cast<size_t>(bytes_per_second_ * interval);
        auto next = std::chrono::steady_clock::now();
        while (running_.load()) {
            size_t taken = ring_.discard(chunk);
            rendered_.fetch_add(taken);
            next += std::chrono::milliseconds(50);
            std::this_thread::sleep_until(next);
        }
    });
}

void NullSink::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

}  // namespace ac3spdif
