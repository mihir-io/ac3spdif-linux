// Live status window: the numbers that tell you whether the stream is
// healthy, the per-channel meters, the latency slider, and the log.
//
// The three figures that matter are buffer, drift and underruns. Buffer should
// sit within a burst or two of the target; drift should be a few hundred ppm
// at most against the ~5200 ppm ceiling; underruns should stay at zero.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engine/config.h"
#include "engine/engine.h"

namespace ac3spdif {

class StatusWindow {
public:
    StatusWindow(GtkApplication* app);

    std::function<void()> on_toggle;
    std::function<void(int)> on_latency_changed;   // bursts; fires on every movement
    // Called by the meter timer to fetch fresh levels.
    std::function<std::vector<float>()> live_levels;

    void update(const EngineStatus& status, int prebuffer_target, int captured_channels, int sample_rate);
    void configure_latency(int bursts, int floor, const std::string& floor_source, int sample_rate, Codec codec);
    void append_log(const std::string& line);
    void show();
    GtkWindow* window() const { return GTK_WINDOW(window_); }

private:
    void build();
    void set_row(const std::string& key, const std::string& value, bool warn = false);
    static gboolean draw_meters(GtkWidget* widget, cairo_t* cr, gpointer data);
    static gboolean meter_tick(gpointer data);
    static void slider_moved(GtkRange* range, gpointer data);
    void refresh_latency_text();

    GtkWidget* window_ = nullptr;
    std::map<std::string, GtkWidget*> rows_;
    GtkWidget* meters_ = nullptr;
    GtkWidget* slider_ = nullptr;
    GtkWidget* slider_readout_ = nullptr;
    GtkWidget* slider_note_ = nullptr;
    GtkWidget* action_button_ = nullptr;
    GtkWidget* log_view_ = nullptr;
    GtkTextBuffer* log_buffer_ = nullptr;
    guint meter_timer_ = 0;

    std::vector<float> levels_;
    std::vector<float> peaks_;
    std::vector<std::string> labels_;
    bool live_ = false;
    gint64 last_meter_update_ = 0;
    bool slider_configuring_ = false;
    int floor_bursts_ = 2;
    std::string floor_source_;
    int sample_rate_ = 48000;
    Codec codec_ = Codec::AC3;
};

}  // namespace ac3spdif
