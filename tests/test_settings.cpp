// Settings round-trip: per-codec values survive a save and load
// independently, the legacy single-value file migrates, and the engine
// config and service arguments use the active codec's values.
#include "app/settings.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace ac3spdif;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    char tmpl[] = "/tmp/ac3spdif-settings-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::puts("cannot create temp dir"); return 1; }
    // Must happen before GLib reads the config dir for the first time.
    setenv("XDG_CONFIG_HOME", dir, 1);

    Settings a;
    a.codec = Codec::AC3;
    a.set_prebuffer_bursts(2);
    a.set_bitrate("448k");
    a.codec = Codec::DTS;
    a.set_prebuffer_bursts(4);
    a.set_bitrate("1411k");
    a.channels = 2;
    a.meters_in_panel = true;
    a.output = "alsa:iec958:CARD=PCH,DEV=0";
    a.save();
    CHECK(g_file_test(Settings::path().c_str(), G_FILE_TEST_EXISTS));

    Settings b;
    b.load();
    CHECK(b.codec == Codec::DTS);
    CHECK(b.prebuffer_bursts() == 4);
    CHECK(b.bitrate() == "1411k");
    b.codec = Codec::AC3;
    CHECK(b.prebuffer_bursts() == 2);
    CHECK(b.bitrate() == "448k");
    CHECK(b.channels == 2);
    CHECK(b.meters_in_panel);
    CHECK(b.output == "alsa:iec958:CARD=PCH,DEV=0");

    // The active codec's values reach the engine and the service.
    b.codec = Codec::DTS;
    Config config = b.to_config();
    CHECK(config.prebuffer_bursts == 4 && config.bitrate == "1411k" && config.codec == Codec::DTS);
    auto args = b.cli_arguments();
    bool prebuffer_ok = false, bitrate_ok = false;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--prebuffer" && args[i + 1] == "4") prebuffer_ok = true;
        if (args[i] == "--bitrate" && args[i + 1] == "1411k") bitrate_ok = true;
    }
    CHECK(prebuffer_ok && bitrate_ok);

    // A file from before the per-codec split: one shared value each, owned
    // by whichever codec was active.
    {
        std::ofstream legacy(Settings::path());
        legacy << "[ac3spdif]\ncodec=dts\nprebuffer_bursts=3\nbitrate=1024k\nchannels=8\n";
    }
    Settings c;
    c.load();
    CHECK(c.codec == Codec::DTS);
    CHECK(c.prebuffer_bursts() == 3);
    CHECK(c.bitrate() == "1024k");
    CHECK(c.channels == 8);
    c.codec = Codec::AC3;
    CHECK(c.prebuffer_bursts() == 6);
    CHECK(c.bitrate().empty());

    // A stale bitrate from the other codec's list is dropped, not carried.
    {
        std::ofstream bad(Settings::path());
        bad << "[ac3spdif]\ncodec=ac3\n[ac3]\nbitrate=1509k\nprebuffer_bursts=99\n";
    }
    Settings d;
    d.load();
    CHECK(d.bitrate().empty());
    CHECK(d.prebuffer_bursts() == 6);

    g_unlink(Settings::path().c_str());
    std::string sub = std::string(dir) + "/ac3spdif";
    g_rmdir(sub.c_str());
    g_rmdir(dir);
    if (failures == 0) std::puts("ok");
    return failures == 0 ? 0 : 1;
}
