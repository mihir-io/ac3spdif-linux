// Run real PCM through the encoder and check that what comes out is a valid
// IEC 61937 stream: whole bursts, the right preamble, the right data type,
// the right burst length for each codec, and a DTS run that does the same.
#include "engine/encoder.h"
#include "engine/ring.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace ac3spdif;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool run_codec(Codec codec, int blocks, int expected_data_type) {
    Config c;
    c.codec = codec;
    c.channels = 6;
    c.prebuffer_bursts = 64;   // never trigger drift correction here
    ByteRing pcm(6 * 4 * 48000 * 2), spdif(static_cast<size_t>(c.burst_bytes()) * 128);
    Encoder encoder(c, pcm, spdif);
    std::string log;
    encoder.on_log = [&](const std::string& line) { log += line + "\n"; };
    encoder.start();

    // A tone per channel, like the self-test.
    const int frames = c.frame_samples() * blocks;
    std::vector<float> audio(static_cast<size_t>(frames) * 6);
    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < 6; ++ch)
            audio[static_cast<size_t>(f) * 6 + ch] = 0.2f * std::sin(2.0 * M_PI * (200 + 100 * ch) * f / 48000.0);
    size_t bytes = audio.size() * sizeof(float);
    size_t written = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (written < bytes && std::chrono::steady_clock::now() < deadline) {
        written += pcm.write(reinterpret_cast<const uint8_t*>(audio.data()) + written, bytes - written);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const size_t expected_bytes = static_cast<size_t>(blocks) * c.burst_bytes();
    while (spdif.fill_level() < expected_bytes && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    encoder.stop();
    if (!log.empty()) std::printf("encoder log: %s", log.c_str());

    std::vector<uint8_t> out(spdif.fill_level());
    spdif.read(out.data(), out.size());
    std::printf("%s: %zu bytes = %.2f bursts\n", codec_short_name(codec), out.size(), double(out.size()) / c.burst_bytes());
    CHECK(out.size() == expected_bytes);
    if (out.size() < static_cast<size_t>(c.burst_bytes())) return false;
    bool ok = true;
    for (int b = 0; b < blocks; ++b) {
        const uint8_t* burst = out.data() + static_cast<size_t>(b) * c.burst_bytes();
        uint16_t pa = burst[0] | (burst[1] << 8), pb = burst[2] | (burst[3] << 8);
        uint16_t pc = burst[4] | (burst[5] << 8), pd = burst[6] | (burst[7] << 8);
        if (pa != 0xF872 || pb != 0x4E1F) { std::printf("burst %d: bad preamble %04x %04x\n", b, pa, pb); ok = false; break; }
        if ((pc & 0x1f) != expected_data_type) { std::printf("burst %d: data type %d\n", b, pc & 0x1f); ok = false; break; }
        int payload_bytes = pd / 8;
        if (payload_bytes <= 0 || payload_bytes > c.burst_bytes() - 8) { std::printf("burst %d: payload %d\n", b, payload_bytes); ok = false; break; }
        if (b == 0) std::printf("  Pc=0x%04x payload %d bytes per burst (%d-byte bursts)\n", pc, payload_bytes, c.burst_bytes());
    }
    CHECK(ok);
    return ok;
}

int main() {
    run_codec(Codec::AC3, 31, 1);    // one second, data type 1 = AC-3
    run_codec(Codec::DTS, 94, 11);   // one second, data type 11 = DTS type I (512 samples)
    if (failures == 0) std::puts("ok");
    return failures == 0 ? 0 : 1;
}
