#include "engine/config.h"

#include <cstdio>

using namespace ac3spdif;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    Config c;
    CHECK(c.effective_bitrate() == "640k");
    c.bitrate = "1509k";
    CHECK(c.bitrate_was_substituted());
    CHECK(c.effective_bitrate() == "640k");
    c.codec = Codec::DTS;
    CHECK(!c.bitrate_was_substituted());
    CHECK(c.effective_bitrate() == "1509k");
    CHECK(c.encode_layout() == "5.1(side)");
    c.codec = Codec::AC3;
    CHECK(c.encode_layout() == "5.1");
    c.channels = 8;
    CHECK(c.channel_layout() == "7.1");
    CHECK(c.encode_layout() == "5.1");
    CHECK(c.downmixes_to_surround51());
    c.channels = 2;
    CHECK(c.encode_layout() == "stereo");

    CHECK(bitrate_bps("640k") == 640000);
    CHECK(bitrate_bps("1411k") == 1411000);
    CHECK(bitrate_bps("nonsense") == 0);
    CHECK(burst_bytes(Codec::AC3) == 6144);
    CHECK(burst_bytes(Codec::DTS) == 2048);

    // Floors measured on the reference hardware.
    CHECK(latency_advice::minimum_bursts(512, Codec::AC3, 48000) == 2);
    CHECK(latency_advice::minimum_bursts(1536, Codec::AC3, 48000) == 2);
    CHECK(latency_advice::minimum_bursts(4096, Codec::AC3, 48000) == 4);
    CHECK(latency_advice::minimum_bursts(512, Codec::DTS, 48000) == 4);
    CHECK(latency_advice::minimum_bursts(4096, Codec::DTS, 48000) == 9);
    CHECK(latency_advice::maximum_bursts(Codec::AC3, 48000) == 12);
    CHECK(latency_advice::maximum_bursts(Codec::DTS, 48000) == 36);
    CHECK(latency_advice::milliseconds(6, 48000, Codec::AC3) == 192);

    Config alsa;
    alsa.output = "alsa:iec958:CARD=PCH,DEV=0";
    CHECK(alsa.output_is_alsa());
    CHECK(alsa.alsa_device() == "iec958:CARD=PCH,DEV=0");
    CHECK(alsa.effective_transport() == Transport::Alsa);
    Config bare;
    bare.output = "hdmi:CARD=HDMI,DEV=0";
    CHECK(bare.output_is_alsa());
    Config none;
    none.output = "NONE";
    CHECK(none.output_is_none());

    bool threw = false;
    try { Config bad; bad.channels = 9; bad.validate(); } catch (const Failure&) { threw = true; }
    CHECK(threw);

    CHECK(parse_codec("DTS") == Codec::DTS);
    CHECK(parse_profile("safe") == LatencyProfile::Safe);
    CHECK(profile_matching(12) == LatencyProfile::Maximum);
    CHECK(!profile_matching(5).has_value());
    CHECK(channel_position_names(6)[3] == "LFE");

    if (failures == 0) std::puts("ok");
    return failures == 0 ? 0 : 1;
}
