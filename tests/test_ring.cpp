// Push 20 MB through a deliberately awkward 7919-byte ring from two threads
// and check that nothing is lost, duplicated or reordered.
#include "engine/ring.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using ac3spdif::ByteRing;

int main() {
    constexpr size_t total = 20 * 1024 * 1024;
    ByteRing ring(7919);
    std::vector<uint8_t> source(total);
    for (size_t i = 0; i < total; ++i) source[i] = static_cast<uint8_t>((i * 7 + (i >> 8)) & 0xff);

    std::thread producer([&] {
        size_t sent = 0;
        unsigned seed = 1;
        while (sent < total) {
            seed = seed * 1103515245u + 12345u;
            size_t chunk = std::min<size_t>(total - sent, 1 + (seed >> 16) % 4096);
            sent += ring.write(source.data() + sent, chunk);
        }
    });

    std::vector<uint8_t> received(total);
    size_t got = 0;
    unsigned seed = 7;
    while (got < total) {
        seed = seed * 1103515245u + 12345u;
        size_t chunk = std::min<size_t>(total - got, 1 + (seed >> 16) % 3000);
        got += ring.read(received.data() + got, chunk);
    }
    producer.join();

    if (received != source) { std::puts("FAIL: data corrupted"); return 1; }
    if (ring.fill_level() != 0) { std::puts("FAIL: ring not empty"); return 1; }

    // Zero padding and discard semantics.
    uint8_t buf[16];
    ring.write("abc", 3);
    size_t shortfall = ring.read_zero_padded(buf, 8);
    if (shortfall != 5 || buf[0] != 'a' || buf[3] != 0) { std::puts("FAIL: zero padding"); return 1; }
    ring.write("0123456789", 10);
    if (ring.discard(4) != 4 || ring.fill_level() != 6) { std::puts("FAIL: discard"); return 1; }
    ring.read(buf, 6);
    if (buf[0] != '4') { std::puts("FAIL: discard offset"); return 1; }
    std::puts("ok");
    return 0;
}
