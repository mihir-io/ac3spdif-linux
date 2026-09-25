#include "app/settings.h"

#include <glib.h>

namespace ac3spdif {

namespace {
const char kGroup[] = "ac3spdif";

const char* codec_group(Codec c) { return c == Codec::AC3 ? "ac3" : "dts"; }
}  // namespace

std::string Settings::path() {
    return std::string(g_get_user_config_dir()) + "/ac3spdif/settings.ini";
}

void Settings::load() {
    GKeyFile* file = g_key_file_new();
    if (g_key_file_load_from_file(file, path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        auto str = [&](const char* group, const char* key, std::string& into) {
            if (gchar* v = g_key_file_get_string(file, group, key, nullptr)) { into = v; g_free(v); return true; }
            return false;
        };
        auto integer = [&](const char* group, const char* key, int& into) {
            GError* error = nullptr;
            int v = g_key_file_get_integer(file, group, key, &error);
            if (error) { g_clear_error(&error); return false; }
            into = v;
            return true;
        };
        auto boolean = [&](const char* group, const char* key, bool& into) {
            GError* error = nullptr;
            gboolean v = g_key_file_get_boolean(file, group, key, &error);
            if (error) { g_clear_error(&error); return false; }
            into = v;
            return true;
        };
        str(kGroup, "input", input);
        str(kGroup, "output", output);
        std::string t, c;
        str(kGroup, "transport", t);
        str(kGroup, "codec", c);
        if (auto parsed = parse_transport(t)) transport = *parsed;
        if (auto parsed = parse_codec(c)) codec = *parsed;
        integer(kGroup, "channels", channels);
        integer(kGroup, "sample_rate", sample_rate);
        integer(kGroup, "device_buffer_frames", device_buffer_frames);
        integer(kGroup, "capture_buffer_frames", capture_buffer_frames);
        boolean(kGroup, "self_test", self_test);
        boolean(kGroup, "set_default_sink", set_default_sink);
        boolean(kGroup, "link_group", link_group);
        boolean(kGroup, "auto_start", auto_start);
        boolean(kGroup, "meters_in_menu", meters_in_menu);
        boolean(kGroup, "meters_in_panel", meters_in_panel);

        for (Codec each : {Codec::AC3, Codec::DTS}) {
            CodecPrefs& p = per_codec[each];
            str(codec_group(each), "bitrate", p.bitrate);
            integer(codec_group(each), "prebuffer_bursts", p.prebuffer_bursts);
            if (!p.bitrate.empty() && !accepts_bitrate(each, p.bitrate)) p.bitrate.clear();
            if (p.prebuffer_bursts < 1 || p.prebuffer_bursts > 64) p.prebuffer_bursts = 6;
        }
        // Files written before the settings were kept per codec had one
        // shared value each; they belonged to whichever codec was in use.
        if (!g_key_file_has_group(file, codec_group(codec))) {
            CodecPrefs& p = per_codec[codec];
            std::string legacy_bitrate;
            if (str(kGroup, "bitrate", legacy_bitrate) && accepts_bitrate(codec, legacy_bitrate)) p.bitrate = legacy_bitrate;
            int legacy_bursts = 0;
            if (integer(kGroup, "prebuffer_bursts", legacy_bursts) && legacy_bursts >= 1 && legacy_bursts <= 64)
                p.prebuffer_bursts = legacy_bursts;
        }
    }
    g_key_file_free(file);
}

void Settings::save() const {
    GKeyFile* file = g_key_file_new();
    g_key_file_set_string(file, kGroup, "input", input.c_str());
    g_key_file_set_string(file, kGroup, "output", output.c_str());
    g_key_file_set_string(file, kGroup, "transport", transport_id(transport));
    g_key_file_set_string(file, kGroup, "codec", codec_id(codec));
    g_key_file_set_integer(file, kGroup, "channels", channels);
    g_key_file_set_integer(file, kGroup, "sample_rate", sample_rate);
    g_key_file_set_integer(file, kGroup, "device_buffer_frames", device_buffer_frames);
    g_key_file_set_integer(file, kGroup, "capture_buffer_frames", capture_buffer_frames);
    g_key_file_set_boolean(file, kGroup, "self_test", self_test);
    g_key_file_set_boolean(file, kGroup, "set_default_sink", set_default_sink);
    g_key_file_set_boolean(file, kGroup, "link_group", link_group);
    g_key_file_set_boolean(file, kGroup, "auto_start", auto_start);
    g_key_file_set_boolean(file, kGroup, "meters_in_menu", meters_in_menu);
    g_key_file_set_boolean(file, kGroup, "meters_in_panel", meters_in_panel);
    for (const auto& [each, p] : per_codec) {
        g_key_file_set_string(file, codec_group(each), "bitrate", p.bitrate.c_str());
        g_key_file_set_integer(file, codec_group(each), "prebuffer_bursts", p.prebuffer_bursts);
    }
    std::string file_path = path();
    gchar* dir = g_path_get_dirname(file_path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    // g_key_file_save_to_file writes to a temporary file and renames it, so
    // a crash mid-write leaves the previous settings intact.
    g_key_file_save_to_file(file, file_path.c_str(), nullptr);
    g_key_file_free(file);
}

Config Settings::to_config() const {
    Config c;
    c.input = input.empty() ? "auto" : input;
    c.output = output;
    c.transport = transport;
    c.codec = codec;
    c.channels = channels;
    c.bitrate = bitrate();
    c.sample_rate = sample_rate;
    c.prebuffer_bursts = prebuffer_bursts();
    c.device_buffer_frames = device_buffer_frames;
    c.capture_buffer_frames = capture_buffer_frames;
    c.self_test = self_test;
    c.set_default_sink = set_default_sink;
    c.link_group = link_group;
    c.status_interval = 0;
    c.quiet = true;
    return c;
}

std::vector<std::string> Settings::cli_arguments() const {
    std::vector<std::string> args{"--quiet", "--status", "0",
                                  "--input", input.empty() ? "auto" : input,
                                  "--transport", transport_id(transport),
                                  "--codec", codec_id(codec),
                                  "--channels", std::to_string(channels),
                                  "--rate", std::to_string(sample_rate),
                                  "--prebuffer", std::to_string(prebuffer_bursts()),
                                  "--device-buffer", std::to_string(device_buffer_frames),
                                  "--capture-buffer", std::to_string(capture_buffer_frames)};
    if (!output.empty()) { args.push_back("--output"); args.push_back(output); }
    if (!bitrate().empty()) { args.push_back("--bitrate"); args.push_back(bitrate()); }
    if (!set_default_sink) args.push_back("--no-default");
    if (!link_group) args.push_back("--no-link-group");
    if (self_test) args.push_back("--selftest");
    return args;
}

}  // namespace ac3spdif
