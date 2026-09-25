// Tray front end.
//
// The app owns an Engine for as long as it is streaming and tears it down on
// quit, which is what "runs only while the app is open" means in practice:
// the default sink and the output device are restored before the process
// exits. Service mode is the same engine driven by the CLI from a systemd user
// unit, and the two are mutually exclusive because whichever takes the sink
// first wins.
//
// The menu is built once and then updated in place. A tray menu is exported
// over D-Bus with an id per item, and the panel caches that tree; replacing
// the menu hands it a tree of new ids, which it re-fetches and rebuilds at
// idle priority, and a submenu open at that moment comes up empty. Labels,
// check marks, visibility and sensitivity are all properties of an existing
// item, so nothing here needs a rebuild except a change in the device list.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/settings.h"
#include "app/status_window.h"
#include "engine/engine.h"
#include "engine/pipewire_session.h"

typedef struct _AppIndicator AppIndicator;

namespace ac3spdif {

struct OutputEntry {
    std::string value;                 // "" automatic, a sink name, or alsa:<pcm>
    GtkWidget* item = nullptr;
    std::optional<NodeInfo> node;      // for sinks, so the capability label can follow the codec
};

struct MenuItems {
    GtkWidget* header = nullptr;
    GtkWidget* stats = nullptr;
    GtkWidget* start_stop = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* meters_separator = nullptr;
    GtkWidget* meters_header = nullptr;
    std::vector<GtkWidget*> meter_rows;
    GtkWidget* meters_in_menu = nullptr;
    GtkWidget* meters_in_panel = nullptr;
    std::vector<std::pair<std::string, GtkWidget*>> capture;     // value, item
    std::vector<OutputEntry> output;
    std::map<int, GtkWidget*> channels;
    std::map<Codec, GtkWidget*> codec;
    GtkWidget* bitrate_auto = nullptr;
    std::vector<std::pair<std::pair<Codec, std::string>, GtkWidget*>> bitrates;
    std::map<Transport, GtkWidget*> transport;
    std::map<int, GtkWidget*> latency;
    GtkWidget* latency_custom = nullptr;
    GtkWidget* self_test = nullptr;
    GtkWidget* route = nullptr;
    GtkWidget* service = nullptr;
    GtkWidget* auto_start = nullptr;
    GtkWidget* login = nullptr;
};

class App {
public:
    explicit App(GtkApplication* application);
    ~App();
    void show_status();
    void quit();

private:
    void build_indicator();
    void build_menu();
    void sync_menu();
    void refresh_stats();
    void maybe_rebuild_for_devices();
    void update_icon(EngineState state);
    void set_meter_timer(bool running);
    void meter_tick();
    std::string meter_row(size_t channel, float level, float peak) const;
    std::string panel_meter(const std::vector<float>& levels) const;
    void start_engine();
    void stop_engine();
    void toggle_engine();
    void restart_if_running();
    void handle_state(EngineState state);
    void append_log(const std::string& line);
    void present_failure(const std::string& message);
    void setting_changed();
    void latency_changed(int bursts);
    void choose_codec(Codec codec);
    void toggle_service();
    int latency_floor(std::string* source) const;
    std::optional<Snapshot> devices();

    GtkApplication* application_;
    Settings settings_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<StatusWindow> window_;
    std::unique_ptr<PwSession> session_;
    AppIndicator* indicator_ = nullptr;
    GtkWidget* menu_ = nullptr;
    MenuItems items_;
    std::optional<Snapshot> snapshot_;
    std::vector<std::string> device_names_;
    std::string icon_dir_;
    guint refresh_timer_ = 0;
    guint latency_commit_timer_ = 0;
    guint meter_timer_ = 0;
    gint64 last_meter_tick_ = 0;
    gint64 last_device_poll_ = 0;
    bool restart_pending_ = false;
    bool quitting_ = false;
    bool panel_label_shown_ = false;
    EngineStatus last_status_;
    EngineState synced_state_ = EngineState::Idle;
    std::vector<float> menu_peaks_;
};

}  // namespace ac3spdif
