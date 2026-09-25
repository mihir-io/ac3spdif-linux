#include "engine/config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ac3spdif {

namespace {
std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
}  // namespace

const char* codec_id(Codec c) { return c == Codec::AC3 ? "ac3" : "dts"; }
const char* codec_name(Codec c) { return c == Codec::AC3 ? "Dolby Digital (AC-3)" : "DTS"; }
const char* codec_short_name(Codec c) { return c == Codec::AC3 ? "AC-3" : "DTS"; }
const char* codec_encoder_name(Codec c) { return c == Codec::AC3 ? "ac3" : "dca"; }
const char* codec_pipewire_name(Codec c) { return c == Codec::AC3 ? "AC3" : "DTS"; }
int frame_samples(Codec c) { return c == Codec::AC3 ? 1536 : 512; }
int burst_bytes(Codec c) { return frame_samples(c) * kCarrierBytesPerFrame; }

// Bitrates the encoder will actually accept.
//
// The two sets do not overlap: the DTS encoder refuses anything at or below
// 640k for 5.1, which is every AC-3 setting, and fails to open rather than
// degrading. AC-3 stops at 640k because A/52 does. DTS stops at 1509k because a
// 2048-byte burst with an 8-byte preamble leaves 2040 bytes for payload; 1536k
// needs all 2048 and the muxer then emits a stream with no sync pattern at all.
const std::vector<std::string>& supported_bitrates(Codec c) {
    static const std::vector<std::string> ac3{"192k", "256k", "384k", "448k", "640k"};
    static const std::vector<std::string> dts{"754k", "1024k", "1280k", "1411k", "1509k"};
    return c == Codec::AC3 ? ac3 : dts;
}

const char* default_bitrate(Codec c) { return c == Codec::AC3 ? "640k" : "1509k"; }

bool accepts_bitrate(Codec c, std::string_view bitrate) {
    const auto& list = supported_bitrates(c);
    return std::find(list.begin(), list.end(), std::string(bitrate)) != list.end();
}

long bitrate_bps(std::string_view bitrate) {
    if (bitrate.empty()) return 0;
    std::string text(bitrate);
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return 0;
    std::string suffix = lower(end);
    if (suffix == "k" || suffix == "kbps") value *= 1000;
    else if (suffix == "m") value *= 1000000;
    else if (!suffix.empty()) return 0;
    return static_cast<long>(std::lround(value));
}

std::string encode_layout(Codec c, int channels) {
    if (channels >= 6) return c == Codec::DTS ? "5.1(side)" : "5.1";
    switch (channels) {
        case 1: return "mono";
        case 2: return "stereo";
        default: return std::to_string(channels) + "c";
    }
}

std::optional<Codec> parse_codec(std::string_view text) {
    std::string t = lower(text);
    if (t == "ac3" || t == "ac-3" || t == "dolby") return Codec::AC3;
    if (t == "dts" || t == "dca") return Codec::DTS;
    return std::nullopt;
}

const char* transport_id(Transport t) {
    switch (t) {
        case Transport::Passthrough: return "passthrough";
        case Transport::Alsa: return "alsa";
        case Transport::Pcm: return "pcm";
    }
    return "?";
}

const char* transport_label(Transport t) {
    switch (t) {
        case Transport::Passthrough: return "PipeWire passthrough (IEC 61937)";
        case Transport::Alsa: return "Direct ALSA, exclusive";
        case Transport::Pcm: return "PCM carrier (16-bit LPCM)";
    }
    return "?";
}

std::optional<Transport> parse_transport(std::string_view text) {
    std::string t = lower(text);
    if (t == "passthrough" || t == "pipewire" || t == "iec958") return Transport::Passthrough;
    if (t == "alsa" || t == "direct" || t == "exclusive") return Transport::Alsa;
    if (t == "pcm") return Transport::Pcm;
    return std::nullopt;
}

int profile_bursts(LatencyProfile p) {
    switch (p) {
        case LatencyProfile::Minimum: return 2;
        case LatencyProfile::Low: return 3;
        case LatencyProfile::Balanced: return 4;
        case LatencyProfile::Safe: return 6;
        case LatencyProfile::Maximum: return 12;
    }
    return 6;
}

const char* profile_id(LatencyProfile p) {
    switch (p) {
        case LatencyProfile::Minimum: return "minimum";
        case LatencyProfile::Low: return "low";
        case LatencyProfile::Balanced: return "balanced";
        case LatencyProfile::Safe: return "safe";
        case LatencyProfile::Maximum: return "maximum";
    }
    return "?";
}

const char* profile_name(LatencyProfile p) {
    switch (p) {
        case LatencyProfile::Minimum: return "Minimum";
        case LatencyProfile::Low: return "Low";
        case LatencyProfile::Balanced: return "Balanced";
        case LatencyProfile::Safe: return "Safe";
        case LatencyProfile::Maximum: return "Maximum";
    }
    return "?";
}

std::optional<LatencyProfile> parse_profile(std::string_view text) {
    std::string t = lower(text);
    for (auto p : kAllProfiles)
        if (t == profile_id(p)) return p;
    return std::nullopt;
}

std::optional<LatencyProfile> profile_matching(int bursts) {
    for (auto p : kAllProfiles)
        if (profile_bursts(p) == bursts) return p;
    return std::nullopt;
}

namespace latency_advice {

int slack_bursts(Codec c, int sample_rate) {
    if (sample_rate <= 0) return kAbsoluteMinimumBursts;
    return static_cast<int>(std::ceil(kMinimumSlackSeconds * sample_rate / frame_samples(c)));
}

int minimum_bursts(int callback_frames, Codec c, int sample_rate) {
    int floor = std::max(kAbsoluteMinimumBursts, slack_bursts(c, sample_rate));
    if (callback_frames <= 0) return floor;
    int bytes_per_callback = callback_frames * kCarrierBytesPerFrame;
    int bursts_per_callback = (bytes_per_callback + burst_bytes(c) - 1) / burst_bytes(c);
    return std::max(floor, bursts_per_callback + 1);
}

int maximum_bursts(Codec c, int sample_rate) {
    if (sample_rate <= 0) return 12;
    return std::max(4, static_cast<int>(std::lround(kMaximumSlackSeconds * sample_rate
                                                    / frame_samples(c))));
}

int milliseconds(int bursts, int sample_rate, Codec c) {
    if (sample_rate <= 0) return 0;
    return static_cast<int>(std::lround(1000.0 * bursts * frame_samples(c) / sample_rate));
}

}  // namespace latency_advice

std::vector<std::string> channel_position_names(int channels) {
    static const std::vector<std::string> order{"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
    if (channels == 1) return {"MONO"};
    std::vector<std::string> out;
    for (int i = 0; i < channels; ++i)
        out.push_back(i < static_cast<int>(order.size()) ? order[i] : "AUX" + std::to_string(i));
    return out;
}

std::string channel_layout_name(int channels) {
    switch (channels) {
        case 1: return "mono";
        case 2: return "stereo";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return std::to_string(channels) + "c";
    }
}

std::string Config::effective_bitrate() const {
    if (!bitrate.empty() && accepts_bitrate(codec, bitrate)) return bitrate;
    return default_bitrate(codec);
}

bool Config::bitrate_was_substituted() const {
    return !bitrate.empty() && !accepts_bitrate(codec, bitrate);
}

std::string Config::channel_layout() const {
    return layout.empty() ? channel_layout_name(channels) : layout;
}

std::string Config::encode_layout() const {
    return ac3spdif::encode_layout(codec, std::min(channels, kMaxEncodedChannels));
}

bool Config::output_is_none() const { return lower(output) == "none"; }

bool Config::output_is_alsa() const {
    if (transport == Transport::Alsa) return true;
    return output.rfind("alsa:", 0) == 0 || output.rfind("iec958:", 0) == 0
        || output.rfind("hdmi:", 0) == 0 || output.rfind("hw:", 0) == 0
        || output.rfind("plughw:", 0) == 0;
}

std::string Config::alsa_device() const {
    if (!output_is_alsa()) return {};
    return output.rfind("alsa:", 0) == 0 ? output.substr(5) : output;
}

Transport Config::effective_transport() const {
    if (output_is_alsa()) return Transport::Alsa;
    return transport == Transport::Alsa ? Transport::Passthrough : transport;
}

void Config::validate() const {
    if (channels < 1 || channels > 8) throw Failure("channels must be 1...8");
    if (std::find(kSupportedSampleRates.begin(), kSupportedSampleRates.end(), sample_rate)
        == kSupportedSampleRates.end())
        throw Failure("sample rate must be 32000, 44100 or 48000 (AC-3 only defines these)");
    if (prebuffer_bursts < 1 || prebuffer_bursts > 64) throw Failure("prebuffer must be 1...64 bursts");
    if (max_drift_frames < 1 || max_drift_frames > 16) throw Failure("max-drift must be 1...16 frames");
    if (device_buffer_frames < 64 || device_buffer_frames > 16384)
        throw Failure("device-buffer must be 64...16384 frames");
    if (capture_buffer_frames < 32 || capture_buffer_frames > 8192)
        throw Failure("capture-buffer must be 32...8192 frames");
    if (!bitrate.empty() && bitrate_bps(bitrate) <= 0) throw Failure("bitrate must look like 640k");
}

}  // namespace ac3spdif
