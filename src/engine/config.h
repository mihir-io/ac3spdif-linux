// Everything the pipeline needs to know, as plain data.
//
// Deliberately free of parsing and UI logic: the CLI builds a Config from
// arguments, the tray app builds one from its settings file, and neither knows
// about the other.
#pragma once

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ac3spdif {

// A user-facing error: the message is the whole story.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Which codec the bitstream carries.
//
// The choice changes the geometry of everything downstream. An AC-3 frame is
// 1536 samples and rides in a 6144-byte IEC 61937 burst; a DTS Type I frame is
// 512 samples in a 2048-byte burst. The output ring is measured, trimmed and
// prebuffered in whole bursts, and a burst is the unit the receiver locks
// onto, so that number propagates through the whole pipeline. Nothing here may
// assume 1536.
enum class Codec { AC3, DTS };

// How the burst reaches the hardware.
//
//   Passthrough  A native PipeWire stream in the codec's IEC 61937 format. The
//                sink switches into encoded mode, the IEC 60958 non-audio
//                channel-status bit is set, and nothing mixes, resamples or
//                scales the stream. The right answer wherever it is offered.
//   Alsa         Open the ALSA device directly and exclusively, asking the
//                sound server to release the card over D-Bus first. The closest
//                thing Linux has to hog mode: no server in the way and the AES
//                bits set on the way in. Takes the whole card away from the
//                desktop for the duration.
//   Pcm          A plain 16-bit stereo stream with the bursts written into it.
//                The non-audio bit is not set, so the receiver has to spot the
//                burst preamble itself. Strictly a fallback.
enum class Transport { Passthrough, Alsa, Pcm };

// How much slack to keep in the output ring, traded against latency.
// Quantised in whole bursts because a burst is exactly one encoded frame and
// the ring can only be trimmed a whole burst at a time without destroying the
// alignment the receiver locks onto.
enum class LatencyProfile { Minimum, Low, Balanced, Safe, Maximum };

constexpr std::array<int, 3> kSupportedSampleRates{32000, 44100, 48000};
// AC-3 and DTS top out at 5.1. 7.1 arrived with E-AC-3 and DTS-HD, which
// S/PDIF does not carry to most receivers.
constexpr int kMaxEncodedChannels = 6;
constexpr int kCarrierBytesPerFrame = 4;   // two 16-bit IEC 60958 subframes
constexpr const char* kDefaultVirtualSink = "ac3spdif";

// --- codec facts -------------------------------------------------------------
const char* codec_id(Codec c);                 // "ac3" | "dts"
const char* codec_name(Codec c);               // "Dolby Digital (AC-3)"
const char* codec_short_name(Codec c);         // "AC-3"
const char* codec_encoder_name(Codec c);       // libavcodec encoder name
const char* codec_pipewire_name(Codec c);      // entry in a sink's iec958.codecs
int frame_samples(Codec c);
int burst_bytes(Codec c);
const std::vector<std::string>& supported_bitrates(Codec c);
const char* default_bitrate(Codec c);
bool accepts_bitrate(Codec c, std::string_view bitrate);
// "640k" -> 640000. Returns 0 when unparsable.
long bitrate_bps(std::string_view bitrate);
// DTS accepts "5.1(side)" but not the back-channel "5.1"; AC-3 takes either.
std::string encode_layout(Codec c, int channels);
std::optional<Codec> parse_codec(std::string_view text);

const char* transport_id(Transport t);
const char* transport_label(Transport t);
std::optional<Transport> parse_transport(std::string_view text);

int profile_bursts(LatencyProfile p);
const char* profile_id(LatencyProfile p);
const char* profile_name(LatencyProfile p);
std::optional<LatencyProfile> parse_profile(std::string_view text);
std::optional<LatencyProfile> profile_matching(int bursts);
constexpr std::array<LatencyProfile, 5> kAllProfiles{
    LatencyProfile::Minimum, LatencyProfile::Low, LatencyProfile::Balanced,
    LatencyProfile::Safe, LatencyProfile::Maximum};

// How small the output buffer can sensibly go, for a given device.
//
// Two effects set the floor and the larger wins. The producer sawtooth: the
// encoder hands over a whole burst at a time while the device drains
// continuously, so the ring swings by about a burst whatever the hardware does,
// and anything less than one spare burst spends part of every cycle empty. The
// callback gulp: one device request must be satisfied in full from what the
// ring is holding, so a device asking for more than a burst per request needs
// the buffer raised to match.
namespace latency_advice {
constexpr int kAbsoluteMinimumBursts = 2;
// Counting bursts alone says AC-3 and DTS both need two, but a DTS burst is
// 10.7 ms against AC-3's 32 ms, so two of them is a third of the cushion
// against scheduling jitter. Forty milliseconds rounds to 2 bursts for AC-3 and
// 4 for DTS, which is what held up on the reference hardware.
constexpr double kMinimumSlackSeconds = 0.040;
constexpr double kMaximumSlackSeconds = 0.384;
int slack_bursts(Codec c, int sample_rate);
int minimum_bursts(int callback_frames, Codec c, int sample_rate);
int maximum_bursts(Codec c, int sample_rate);
int milliseconds(int bursts, int sample_rate, Codec c);
}  // namespace latency_advice

// The channel order the encoder is fed, as PipeWire position names. They match
// libavcodec's layouts of the same width, so the interleaved capture goes
// straight into the encoder with no remapping in between.
std::vector<std::string> channel_position_names(int channels);
std::string channel_layout_name(int channels);   // "5.1", "stereo", ...

struct Config {
    // "auto" creates and captures the managed virtual sink; anything else
    // names a PipeWire source or sink (a sink is captured from its monitor).
    std::string input = "auto";
    // Empty picks the best passthrough-capable digital sink. "none" encodes and
    // discards. "alsa:<pcm>" or a bare ALSA PCM name selects the direct
    // transport.
    std::string output;
    Transport transport = Transport::Passthrough;
    Codec codec = Codec::AC3;
    int channels = 6;
    std::string layout;                 // explicit ffmpeg layout override
    std::string bitrate;                // empty = codec default
    int sample_rate = 48000;
    int prebuffer_bursts = 6;
    int max_drift_frames = 8;
    // Sink-side buffer in frames: the PipeWire node latency of the output
    // stream, or the ALSA buffer. Small on purpose; the ring upstream does the
    // buffering, and it is the part whose size is a choice.
    int device_buffer_frames = 512;
    // Capture stream latency in frames.
    int capture_buffer_frames = 256;
    double status_interval = 5.0;
    bool quiet = false;
    bool show_meters = false;
    bool self_test = false;
    std::string dump_path;
    std::string virtual_sink_name = kDefaultVirtualSink;
    // Make the virtual sink the system default while streaming, and put the
    // previous default back afterwards.
    bool set_default_sink = true;
    // Ask PipeWire to schedule capture and playback under one driver, so the
    // two sides share a clock and the drift controller has nothing to do.
    bool link_group = true;

    std::string effective_bitrate() const;
    bool bitrate_was_substituted() const;
    int frame_samples() const { return ac3spdif::frame_samples(codec); }
    int burst_bytes() const { return ac3spdif::burst_bytes(codec); }
    std::string channel_layout() const;   // what the capture is described as
    std::string encode_layout() const;    // what the encoder emits
    bool downmixes_to_surround51() const { return channels > kMaxEncodedChannels; }
    bool output_is_none() const;
    bool output_is_alsa() const;
    std::string alsa_device() const;      // the PCM behind an alsa: output
    Transport effective_transport() const;
    void validate() const;
};

}  // namespace ac3spdif
