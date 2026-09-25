// ac3spdif: command-line front end.
//
// All the work lives in the engine; this file is argument handling, a status
// line, and signal-driven shutdown. The tray app is the same shape with a
// different surface.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "engine/alsa_devices.h"
#include "engine/config.h"
#include "engine/engine.h"
#include "engine/log.h"
#include "engine/pipewire_session.h"
#include "engine/version.h"

using namespace ac3spdif;

namespace {

const char kUsage[] = R"(ac3spdif - real-time Dolby Digital Live / DTS Connect for Linux

Captures all system audio from a virtual PipeWire sink, encodes it to AC-3 or
DTS in real time, and bitstreams the result over S/PDIF or HDMI as an IEC 61937
data burst, so the receiver decodes the surround mix itself.

USAGE
  ac3spdif [options]
  ac3spdif --list

OPTIONS
  --input <auto|name>   Capture source          (default: auto = a managed virtual sink)
                        A sink name captures its monitor; a source name captures it.
  --output <name>       Digital sink to stream to (default: best passthrough-capable sink)
  --output alsa:<pcm>   Direct ALSA transport, e.g. alsa:iec958:CARD=PCH,DEV=0
  --output none         Encode and discard - diagnose capture, claim no device
  --transport <mode>    passthrough | pcm | alsa (default: passthrough)
  --codec <name>        ac3 | dts               (default: ac3)
  --bitrate <rate>      Encoder bitrate         (default: per codec)
                        AC-3 and DTS accept different values and the sets do
                        not overlap; an unusable one is replaced with the codec
                        default rather than failing to start.
  --channels <n>        Channels to capture     (default: 6, encoded as at most 5.1)
  --layout <layout>     ffmpeg channel layout   (default: derived from --channels)
  --rate <hz>           Sample rate             (default: 48000)
  --prebuffer <bursts>  Output ring slack       (default: 6)
  --latency <profile>   minimum | low | balanced | safe | maximum
  --max-drift <frames>  Max drift correction    (default: 8 = ~5200 ppm)
  --device-buffer <n>   Sink-side buffer, frames (default: 512)
  --capture-buffer <n>  Capture stream latency, frames (default: 256)
  --no-default          Do not make the virtual sink the system default
  --no-link-group       Do not ask PipeWire to clock capture from the output
  --status <seconds>    Status line interval    (default: 5, 0 to disable)
  --quiet               Suppress status lines
  --meters              Show a per-channel input level meter
  --selftest            Emit a distinct tone per channel instead of capturing
  --dump <path>         Also write the IEC 61937 stream to a file
  --list                List sinks, sources and ALSA devices with their bitstream capability
  --version             Print the version and exit
  --help                This text

TRANSPORTS
  passthrough  A native PipeWire stream in the codec's IEC 61937 format. The
               sink switches into encoded mode, the IEC 60958 non-audio bit is
               set, and nothing mixes, resamples or scales the burst. A sink
               that does not list the codec yet has it enabled on the way in.
  alsa         Opens the ALSA device directly and exclusively after asking
               WirePlumber to release the card over D-Bus. Lowest latency, no
               sound server in the path; the whole card leaves the desktop
               while streaming and comes back on exit.
  pcm          A plain 16-bit stereo stream carrying the bursts. The non-audio
               bit is not set, so the receiver must spot the preamble itself.
               Strictly a fallback.

LATENCY
  The output ring is the one latency knob worth turning. --latency picks a
  size by name, --prebuffer sets it in bursts directly. A burst is one encoded
  frame: 32 ms for AC-3, 10.7 ms for DTS at 48 kHz.

    minimum    2 bursts   low        3 bursts   balanced   4 bursts
    safe       6 bursts   (default)  maximum   12 bursts

  The floor depends on the device and the codec; the engine warns at start-up
  when the buffer is below it. Watch the underruns figure: if it climbs, the
  buffer is too small for this machine.

CODECS
  ac3   Dolby Digital. 1536 samples per frame in a 6144-byte burst.
  dts   DTS Coherent Acoustics via ffmpeg's experimental encoder. 512 samples
        per frame in a 2048-byte burst, a third of AC-3's latency per burst.
)";

struct Arguments {
    Config config;
    bool list = false;
    bool help = false;
};

Arguments parse(int argc, char** argv) {
    Arguments a;
    Config& c = a.config;
    auto need = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) throw Failure(fmt("{} needs a value", flag));
        return argv[++i];
    };
    auto as_int = [&](const std::string& text, const char* flag) {
        char* end = nullptr;
        long value = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end) throw Failure(fmt("{} needs a number", flag));
        return static_cast<int>(value);
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input") c.input = need(i, "--input");
        else if (arg == "--output") c.output = need(i, "--output");
        else if (arg == "--transport") {
            auto t = parse_transport(need(i, "--transport"));
            if (!t) throw Failure("--transport must be passthrough, pcm or alsa");
            c.transport = *t;
        } else if (arg == "--codec") {
            auto codec = parse_codec(need(i, "--codec"));
            if (!codec) throw Failure("--codec must be ac3 or dts");
            c.codec = *codec;
        } else if (arg == "--bitrate") c.bitrate = need(i, "--bitrate");
        else if (arg == "--channels") c.channels = as_int(need(i, "--channels"), "--channels");
        else if (arg == "--layout") c.layout = need(i, "--layout");
        else if (arg == "--rate") c.sample_rate = as_int(need(i, "--rate"), "--rate");
        else if (arg == "--prebuffer") c.prebuffer_bursts = as_int(need(i, "--prebuffer"), "--prebuffer");
        else if (arg == "--latency") {
            auto profile = parse_profile(need(i, "--latency"));
            if (!profile) throw Failure("--latency must be minimum, low, balanced, safe or maximum");
            c.prebuffer_bursts = profile_bursts(*profile);
        } else if (arg == "--max-drift") c.max_drift_frames = as_int(need(i, "--max-drift"), "--max-drift");
        else if (arg == "--device-buffer") c.device_buffer_frames = as_int(need(i, "--device-buffer"), "--device-buffer");
        else if (arg == "--capture-buffer") c.capture_buffer_frames = as_int(need(i, "--capture-buffer"), "--capture-buffer");
        else if (arg == "--no-default") c.set_default_sink = false;
        else if (arg == "--no-link-group") c.link_group = false;
        else if (arg == "--sink-name") c.virtual_sink_name = need(i, "--sink-name");
        else if (arg == "--status") c.status_interval = std::strtod(need(i, "--status").c_str(), nullptr);
        else if (arg == "--quiet") c.quiet = true;
        else if (arg == "--meters") c.show_meters = true;
        else if (arg == "--selftest") c.self_test = true;
        else if (arg == "--dump") c.dump_path = need(i, "--dump");
        else if (arg == "--list") a.list = true;
        else if (arg == "--version") { std::printf("ac3spdif %s\n", AC3SPDIF_VERSION); std::exit(0); }
        else if (arg == "--help" || arg == "-h") a.help = true;
        else throw Failure("unknown option " + arg);
    }
    c.validate();
    return a;
}

void list_devices(const Config& config) {
    PwSession session;
    Snapshot snap = session.snapshot();
    std::printf("PipeWire %s (%s)\n", snap.server_version.c_str(), snap.server_name.c_str());
    std::printf("default sink: %s\n\n", snap.default_sink.c_str());
    std::printf("Sinks (--output):\n");
    for (const NodeInfo* sink : snap.sinks()) {
        std::string codecs;
        for (const auto& c : sink->iec958_codecs) codecs += (codecs.empty() ? "" : " ") + c;
        std::string port = port_type_of(snap, *sink);
        std::printf("  %-14s %s\n", capability_label(snap, *sink, config.codec).c_str(), sink->display_name().c_str());
        std::printf("      name: %s\n", sink->name.c_str());
        std::printf("      port: %s   codecs: %s   api: %s%s\n", port.empty() ? "?" : port.c_str(),
                    codecs.empty() ? "-" : codecs.c_str(), sink->api().c_str(),
                    sink->is_virtual() ? "   (virtual)" : "");
    }
    std::printf("\nSources (--input):\n");
    for (const NodeInfo* source : snap.sources()) {
        std::printf("  %s  [%d ch]\n      name: %s\n", source->display_name().c_str(), source->channels, source->name.c_str());
    }
    std::printf("  Any sink above can be captured from its monitor by giving its name.\n");
    auto pcms = alsa_digital_pcms();
    std::printf("\nALSA digital outputs (--output alsa:<name>):\n");
    if (pcms.empty()) std::printf("  none found\n");
    for (const auto& pcm : pcms) {
        std::printf("  %s\n      %s", pcm.name.c_str(), pcm.description.c_str());
        std::string status = alsa_iec958_status(pcm.card_index);
        if (!status.empty()) std::printf("\n      status: %s", status.c_str());
        std::printf("\n");
    }
    auto ranked = ranked_bitstream_sinks(snap, config.codec, false);
    if (ranked.empty())
        std::printf("\nNo sink can be selected automatically for %s. Name one with --output.\n", codec_short_name(config.codec));
    else
        std::printf("\nAutomatic choice for %s: %s (%s)\n", codec_short_name(config.codec),
                    ranked.front().node.display_name().c_str(), ranked.front().reason.c_str());
}

volatile std::sig_atomic_t g_shutdown = 0;
void on_signal(int) { g_shutdown = 1; }

std::string meter_line(const std::vector<float>& levels, int channels) {
    static const char* ramp[] = {"·", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    auto names = channel_position_names(channels);
    std::string out;
    for (size_t i = 0; i < levels.size(); ++i) {
        float db = levels[i] <= 0 ? -60.0f : std::max(-60.0f, 20.0f * std::log10(levels[i]));
        int step = static_cast<int>(std::lround((db + 60.0f) / 60.0f * 8));
        step = std::clamp(step, 0, 8);
        out += (i ? " " : "") + (i < names.size() ? names[i] : std::to_string(i + 1)) + ramp[step];
    }
    return out;
}

void print_summary(const EngineStatus& status, const Config& config) {
    const auto& hold = status.input_peak_hold;
    if (hold.empty()) return;
    auto names = channel_position_names(config.channels);
    bool any = false;
    std::string row;
    for (size_t i = 0; i < hold.size(); ++i) {
        if (hold[i] > 0.005f) any = true;
        row += fmt("{}{} {:.3f}", i ? "   " : "", i < names.size() ? names[i] : std::to_string(i + 1), hold[i]);
    }
    if (!any) {
        std::fprintf(stderr, "summary : no audio arrived on any channel of %s during this run.\n", status.input_name.c_str());
        std::fprintf(stderr, "summary : check that it is the system output, and that something was playing.\n");
        return;
    }
    std::fprintf(stderr, "summary : peak level per channel over the whole run\nsummary :   %s\n", row.c_str());
}

int run(const Config& config) {
    std::mutex log_mutex;
    std::deque<std::string> log_lines;
    Engine engine(config);
    engine.on_log = [&](const std::string& line) {
        std::lock_guard<std::mutex> guard(log_mutex);
        log_lines.push_back(line);
    };
    auto drain_logs = [&] {
        std::deque<std::string> lines;
        {
            std::lock_guard<std::mutex> guard(log_mutex);
            lines.swap(log_lines);
        }
        for (const auto& line : lines)
            if (!config.quiet) std::fprintf(stderr, "note    : %s\n", line.c_str());
    };
    bool announced = false;
    engine.start();

    auto next_status = std::chrono::steady_clock::now() + std::chrono::duration<double>(config.status_interval);
    auto next_meter = std::chrono::steady_clock::now();
    bool printed_breakdown = false;
    while (!g_shutdown && state_is_active(engine.state())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        drain_logs();
        EngineStatus status = engine.status();
        if (status.state == EngineState::Running && !announced && !config.quiet) {
            announced = true;
            std::fprintf(stderr, "capture : %s - %dch (%s) @ %d Hz\n", status.input_name.c_str(), config.channels,
                         config.channel_layout().c_str(), config.sample_rate);
            std::fprintf(stderr, "encode  : %s %s, %s frames\n", codec_name(config.codec), config.effective_bitrate().c_str(),
                         config.encode_layout().c_str());
            std::fprintf(stderr, "output  : %s (%s) - transport %s, %s\n", status.output_name.c_str(), status.stream.c_str(),
                         status.carrier.c_str(), status.format.c_str());
            if (!status.sink_format.empty()) std::fprintf(stderr, "sink    : negotiated %s\n", status.sink_format.c_str());
            if (!config.dump_path.empty())
                std::fprintf(stderr, "dump    : %s - decode with `ffmpeg -f spdif -i %s out.wav`\n", config.dump_path.c_str(), config.dump_path.c_str());
            std::fprintf(stderr, "running : bitstreaming %s - press Ctrl-C to stop\n", codec_name(config.codec));
        }
        auto now = std::chrono::steady_clock::now();
        if (config.show_meters && status.state == EngineState::Running && now >= next_meter) {
            next_meter = now + std::chrono::milliseconds(400);
            auto levels = engine.live_input_levels();
            if (!levels.empty()) std::fprintf(stderr, "meters  : %s\n", meter_line(levels, config.channels).c_str());
        }
        if (config.status_interval <= 0 || config.quiet || status.state != EngineState::Running || now < next_status) continue;
        next_status = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(config.status_interval));
        const LatencyBreakdown& l = status.latency;
        if (!printed_breakdown && l.total() > 0) {
            printed_breakdown = true;
            auto row = [&](const char* label, int frames, const char* note) {
                std::fprintf(stderr, "latency : %-18s %6d frames  %6.1f ms   %s\n", label, frames,
                             LatencyBreakdown::ms(frames, config.sample_rate), note);
            };
            row("capture", l.capture_frames, "stream quantum plus reported delay");
            row("pcm queue", l.pcm_ring_frames, "transient");
            row(config.codec == Codec::AC3 ? "AC-3 frame" : "DTS frame", l.encoder_frames, "inherent to the format");
            row("output ring", l.output_ring_frames, "what --prebuffer controls");
            row("device", l.device_frames, "reported by the output");
            std::fprintf(stderr, "latency : %-18s %6d frames  %6.1f ms   end to end\n", "TOTAL", l.total(),
                         LatencyBreakdown::ms(l.total(), config.sample_rate));
        }
        std::fprintf(stderr, "status  : %.1f s on the wire | %llu bursts | buffer %d bursts | latency %.0f ms | drift %+.0f ppm | underruns %llu B | capture overflow %llu frames%s\n",
                     status.seconds_on_wire, static_cast<unsigned long long>(status.bursts), status.buffer_bursts,
                     LatencyBreakdown::ms(l.total(), config.sample_rate), status.drift_ppm,
                     static_cast<unsigned long long>(status.underrun_bytes),
                     static_cast<unsigned long long>(status.capture_overflow_frames),
                     status.silence_blocks ? fmt(" | silence blocks {}", status.silence_blocks).c_str() : "");
    }

    EngineStatus final_status = engine.status();
    if (!config.quiet && state_is_active(engine.state()))
        std::fprintf(stderr, "stopping: restoring the default sink and releasing the output\n");
    engine.stop();
    engine.wait_until_stopped(std::chrono::seconds(10));
    drain_logs();
    if (config.show_meters && !config.quiet) print_summary(final_status, config);
    EngineStatus last = engine.status();
    if (last.state == EngineState::Failed) {
        std::fprintf(stderr, "ac3spdif: %s\n", last.failure.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    try {
        Arguments args = parse(argc, argv);
        if (args.help) { std::fputs(kUsage, stdout); return 0; }
        if (args.list) { list_devices(args.config); return 0; }
        return run(args.config);
    } catch (const Failure& failure) {
        std::fprintf(stderr, "ac3spdif: %s\n", failure.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ac3spdif: %s\n", error.what());
        return 1;
    }
}
