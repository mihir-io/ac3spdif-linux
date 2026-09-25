// The app's settings, kept in a key file under the XDG config directory and
// translated into an engine Config on demand. The engine knows nothing about
// persistence; this is the only place that does.
//
// Every change is written out at once, so quitting, logging out or a power
// cut loses nothing. The two settings whose right value depends on the codec,
// the output buffer and the bitrate, are kept per codec: a buffer that is the
// floor for AC-3 is below it for DTS, and the bitrate lists do not even
// overlap, so one shared value would be wrong for whichever codec was not in
// use when it was chosen.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "engine/config.h"

namespace ac3spdif {

struct CodecPrefs {
    std::string bitrate;        // empty = codec default
    int prebuffer_bursts = 6;   // the "safe" profile for either codec
};

struct Settings {
    std::string input = "auto";
    std::string output;                 // empty = automatic
    Transport transport = Transport::Passthrough;
    Codec codec = Codec::AC3;
    int channels = 6;
    int sample_rate = 48000;
    int device_buffer_frames = 512;
    int capture_buffer_frames = 256;
    bool self_test = false;
    bool set_default_sink = true;
    bool link_group = true;
    bool auto_start = false;            // start streaming when the app opens
    bool meters_in_menu = true;         // live channel rows in the tray menu
    bool meters_in_panel = false;       // one glyph per channel beside the icon
    std::map<Codec, CodecPrefs> per_codec{{Codec::AC3, {}}, {Codec::DTS, {}}};

    // The active codec's own values.
    const CodecPrefs& prefs() const { return per_codec.at(codec); }
    CodecPrefs& prefs() { return per_codec[codec]; }
    int prebuffer_bursts() const { return prefs().prebuffer_bursts; }
    void set_prebuffer_bursts(int bursts) { prefs().prebuffer_bursts = bursts; }
    std::string bitrate() const { return prefs().bitrate; }
    void set_bitrate(const std::string& value) { prefs().bitrate = value; }

    std::string virtual_sink_name_or_default() const { return kDefaultVirtualSink; }
    static std::string path();
    void load();
    void save() const;
    Config to_config() const;
    // The arguments that reproduce this configuration on the command line,
    // for the background service.
    std::vector<std::string> cli_arguments() const;
};

}  // namespace ac3spdif
