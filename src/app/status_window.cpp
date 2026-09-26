#include "app/status_window.h"

#include <cmath>
#include <ctime>

#include "engine/log.h"
#include "engine/version.h"

namespace ac3spdif {

namespace {

const char* kRows[] = {"State", "Capture", "Output", "Sink format", "Transport", "On the wire", "Bursts",
                       "Buffer", "Drift", "Underruns", "Capture overflow", "Latency", "Channels seen", "Build"};

constexpr float kFloorDb = -60.0f;

float to_db(float linear) { return linear <= 0 ? kFloorDb : std::max(kFloorDb, 20.0f * std::log10(linear)); }
float from_db(float db) { return db <= kFloorDb ? 0.0f : std::pow(10.0f, db / 20.0f); }
// Position along the bar on the dB scale: a linear meter spends most of its
// travel on the top 6 dB and shows nothing at conversational levels.
double position(float linear) { return (to_db(linear) - kFloorDb) / -kFloorDb; }

}  // namespace

StatusWindow::StatusWindow(GtkApplication* app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), fmt("ac3spdif {}", AC3SPDIF_VERSION).c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), 560, 640);
    gtk_window_set_icon_name(GTK_WINDOW(window_), AC3SPDIF_APP_ID);
    g_signal_connect(window_, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
    build();
}

void StatusWindow::build() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(root), 16);
    gtk_container_add(GTK_CONTAINER(window_), root);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    int row = 0;
    for (const char* title : kRows) {
        GtkWidget* label = gtk_label_new(title);
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
        pango_attr_list_insert(attrs, pango_attr_scale_new(0.9));
        gtk_label_set_attributes(GTK_LABEL(label), attrs);
        pango_attr_list_unref(attrs);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
        GtkWidget* value = gtk_label_new("—");
        gtk_widget_set_halign(value, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_selectable(GTK_LABEL(value), TRUE);
        gtk_widget_set_hexpand(value, TRUE);
        PangoAttrList* mono = pango_attr_list_new();
        pango_attr_list_insert(mono, pango_attr_family_new("monospace"));
        pango_attr_list_insert(mono, pango_attr_scale_new(0.9));
        gtk_label_set_attributes(GTK_LABEL(value), mono);
        pango_attr_list_unref(mono);
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
        rows_[title] = value;
        ++row;
    }
    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    // Meters: the channels going *into* the encoder. Metering the output
    // would be meaningless; by then it is a bitstream whose "levels" are the
    // entropy of compressed data.
    GtkWidget* meter_label = gtk_label_new("Channel activity");
    gtk_widget_set_halign(meter_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(meter_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(root), meter_label, FALSE, FALSE, 0);
    meters_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(meters_, -1, 6 * 18 + 8);
    g_signal_connect(meters_, "draw", G_CALLBACK(draw_meters), this);
    gtk_box_pack_start(GTK_BOX(root), meters_, FALSE, FALSE, 0);

    // The latency slider, in whole bursts, because that is the only unit the
    // ring can be adjusted in.
    GtkWidget* latency_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    slider_readout_ = gtk_label_new("");
    gtk_widget_set_halign(slider_readout_, GTK_ALIGN_START);
    slider_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 12, 1);
    gtk_scale_set_draw_value(GTK_SCALE(slider_), FALSE);
    gtk_scale_set_digits(GTK_SCALE(slider_), 0);
    g_signal_connect(slider_, "value-changed", G_CALLBACK(slider_moved), this);
    slider_note_ = gtk_label_new("");
    gtk_widget_set_halign(slider_note_, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(slider_note_), "dim-label");
    gtk_box_pack_start(GTK_BOX(latency_box), slider_readout_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(latency_box), slider_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(latency_box), slider_note_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), latency_box, FALSE, FALSE, 0);

    action_button_ = gtk_button_new_with_label("Start");
    gtk_widget_set_halign(action_button_, GTK_ALIGN_START);
    g_signal_connect_swapped(action_button_, "clicked", G_CALLBACK(+[](gpointer data) {
        auto* self = static_cast<StatusWindow*>(data);
        if (self->on_toggle) self->on_toggle();
    }), this);
    gtk_box_pack_start(GTK_BOX(root), action_button_, FALSE, FALSE, 0);

    GtkWidget* log_label = gtk_label_new("Log");
    gtk_widget_set_halign(log_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(log_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(root), log_label, FALSE, FALSE, 0);
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 160);
    log_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view_), GTK_WRAP_WORD_CHAR);
    log_buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view_));
    gtk_container_add(GTK_CONTAINER(scroll), log_view_);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(root);
}

void StatusWindow::set_row(const std::string& key, const std::string& value, bool warn) {
    auto it = rows_.find(key);
    if (it == rows_.end()) return;
    gtk_label_set_text(GTK_LABEL(it->second), value.c_str());
    GtkStyleContext* style = gtk_widget_get_style_context(it->second);
    if (warn) gtk_style_context_add_class(style, "error");
    else gtk_style_context_remove_class(style, "error");
}

void StatusWindow::update(const EngineStatus& status, int prebuffer_target, int captured_channels, int sample_rate) {
    if (status.state == EngineState::Failed) set_row("State", "Failed — " + status.failure, true);
    else set_row("State", state_label(status.state));
    set_row("Capture", status.input_name.empty() ? "—" : status.input_name);
    set_row("Output", status.output_name.empty() ? "—" : status.output_name);
    set_row("Sink format", status.sink_format.empty() ? "—" : status.sink_format);
    set_row("Transport", status.carrier.empty() ? "—" : status.carrier + "  " + status.format,
            status.carrier == "pcm");
    set_row("On the wire", fmt("{:.1f} s", status.seconds_on_wire));
    set_row("Bursts", std::to_string(status.bursts));
    // Drifting far from the target is the early warning that the drift
    // controller is losing; underruns are what you hear when it has lost.
    set_row("Buffer", fmt("{} / {} bursts", status.buffer_bursts, prebuffer_target),
            status.state == EngineState::Running && std::abs(status.buffer_bursts - prebuffer_target) > 3);
    set_row("Drift", fmt("{:+.0f} ppm", status.drift_ppm), std::fabs(status.drift_ppm) > 4000);
    set_row("Underruns", fmt("{} B", status.underrun_bytes), status.underrun_bytes > 0);
    set_row("Capture overflow", fmt("{} frames", status.capture_overflow_frames), status.capture_overflow_frames > 0);
    const LatencyBreakdown& l = status.latency;
    set_row("Latency", l.total() > 0
                ? fmt("{:.0f} ms  (capture {:.0f} + frame {:.0f} + ring {:.0f} + device {:.0f})",
                      LatencyBreakdown::ms(l.total(), sample_rate), LatencyBreakdown::ms(l.capture_frames, sample_rate),
                      LatencyBreakdown::ms(l.encoder_frames, sample_rate), LatencyBreakdown::ms(l.output_ring_frames, sample_rate),
                      LatencyBreakdown::ms(l.device_frames, sample_rate))
                : "—");
    // The undecayed record: still answers "which channel was that?" after
    // playback has stopped and the live meters read silence.
    std::string seen;
    auto names = channel_position_names(captured_channels);
    for (size_t i = 0; i < status.input_peak_hold.size(); ++i)
        if (status.input_peak_hold[i] > 0.005f) seen += (seen.empty() ? "" : " ") + (i < names.size() ? names[i] : std::to_string(i + 1));
    set_row("Channels seen", seen.empty() ? "none yet" : seen);
    set_row("Build", fmt("{} · {} {}", AC3SPDIF_VERSION, __DATE__, __TIME__));

    live_ = status.state == EngineState::Running;
    labels_ = names;
    gtk_button_set_label(GTK_BUTTON(action_button_), state_is_active(status.state) ? "Stop" : "Start");
    gtk_widget_set_sensitive(action_button_, status.state != EngineState::Starting && status.state != EngineState::Stopping);
    if (live_ && meter_timer_ == 0 && gtk_widget_get_visible(window_))
        meter_timer_ = g_timeout_add(33, meter_tick, this);
    if (!live_) {
        levels_.assign(names.size(), 0.0f);
        peaks_.assign(names.size(), 0.0f);
        gtk_widget_queue_draw(meters_);
    }
    gtk_widget_set_size_request(meters_, -1, static_cast<int>(names.size()) * 18 + 8);
}

gboolean StatusWindow::meter_tick(gpointer data) {
    auto* self = static_cast<StatusWindow*>(data);
    if (!self->live_ || !gtk_widget_get_visible(self->window_)) {
        self->meter_timer_ = 0;
        return G_SOURCE_REMOVE;
    }
    std::vector<float> levels = self->live_levels ? self->live_levels() : std::vector<float>{};
    size_t wanted = self->labels_.size();
    levels.resize(wanted, 0.0f);
    if (self->peaks_.size() != wanted) self->peaks_ = levels;
    // Peak marks fall at a fixed rate in dB per second rather than by a
    // per-frame multiplier, so the decay looks the same however often the
    // window happens to redraw.
    gint64 now = g_get_monotonic_time();
    float elapsed = self->last_meter_update_ ? std::min(0.5f, static_cast<float>(now - self->last_meter_update_) / 1e6f) : 0.033f;
    self->last_meter_update_ = now;
    float fall_db = 24.0f * elapsed;
    for (size_t i = 0; i < wanted; ++i) {
        float decayed = from_db(to_db(self->peaks_[i]) - fall_db);
        self->peaks_[i] = std::max(levels[i], std::min(self->peaks_[i], decayed));
    }
    self->levels_ = levels;
    gtk_widget_queue_draw(self->meters_);
    return G_SOURCE_CONTINUE;
}

gboolean StatusWindow::draw_meters(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* self = static_cast<StatusWindow*>(data);
    GtkStyleContext* style = gtk_widget_get_style_context(widget);
    GdkRGBA fg;
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &fg);
    const double width = gtk_widget_get_allocated_width(widget);
    const double row_height = 18, label_width = 34, readout_width = 44, inset = 4;
    const double bar_left = inset + label_width + 6, bar_right = width - inset - readout_width - 6;
    const double bar_width = std::max(bar_right - bar_left, 10.0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    for (size_t i = 0; i < self->labels_.size(); ++i) {
        double centre = inset + row_height * (i + 0.5);
        float level = i < self->levels_.size() ? self->levels_[i] : 0.0f;
        float peak = i < self->peaks_.size() ? self->peaks_[i] : 0.0f;
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.75);
        cairo_move_to(cr, inset, centre + 4);
        cairo_show_text(cr, self->labels_[i].c_str());
        // Track.
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.12);
        cairo_rectangle(cr, bar_left, centre - 3.5, bar_width, 7);
        cairo_fill(cr);
        if (self->live_) {
            double filled = bar_width * position(level);
            if (filled > 1) {
                // Red near full scale, where a capture is about to clip.
                if (to_db(level) > -1.0f) cairo_set_source_rgb(cr, 0.88, 0.11, 0.14);
                else cairo_set_source_rgb(cr, 0.21, 0.52, 0.89);
                cairo_rectangle(cr, bar_left, centre - 3.5, filled, 7);
                cairo_fill(cr);
            }
            double peak_x = bar_left + bar_width * position(peak);
            if (peak > 0 && peak_x > bar_left + 1) {
                cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.75);
                cairo_rectangle(cr, std::min(peak_x, bar_right - 1.5), centre - 3.5, 1.5, 7);
                cairo_fill(cr);
            }
        }
        float db = to_db(level);
        std::string readout = !self->live_ ? "—" : db <= kFloorDb ? "-∞" : fmt("{:.0f}", std::round(db) == 0 ? 0.0 : std::round(db));
        cairo_text_extents_t extents;
        cairo_text_extents(cr, readout.c_str(), &extents);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, self->live_ ? 0.75 : 0.4);
        cairo_move_to(cr, width - inset - extents.x_advance, centre + 4);
        cairo_show_text(cr, readout.c_str());
    }
    return FALSE;
}

void StatusWindow::configure_latency(int bursts, int floor, const std::string& floor_source, int sample_rate, Codec codec) {
    floor_bursts_ = floor;
    floor_source_ = floor_source;
    sample_rate_ = sample_rate;
    codec_ = codec;
    int maximum = latency_advice::maximum_bursts(codec, sample_rate);
    slider_configuring_ = true;
    gtk_range_set_range(GTK_RANGE(slider_), 1, maximum);
    gtk_scale_clear_marks(GTK_SCALE(slider_));
    for (int b = 1; b <= maximum; ++b) gtk_scale_add_mark(GTK_SCALE(slider_), b, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_value(GTK_RANGE(slider_), std::clamp(bursts, 1, maximum));
    slider_configuring_ = false;
    refresh_latency_text();
}

void StatusWindow::slider_moved(GtkRange* range, gpointer data) {
    auto* self = static_cast<StatusWindow*>(data);
    int bursts = static_cast<int>(std::lround(gtk_range_get_value(range)));
    self->refresh_latency_text();
    if (!self->slider_configuring_ && self->on_latency_changed) self->on_latency_changed(bursts);
}

void StatusWindow::refresh_latency_text() {
    int bursts = static_cast<int>(std::lround(gtk_range_get_value(GTK_RANGE(slider_))));
    int ms = latency_advice::milliseconds(bursts, sample_rate_, codec_);
    gtk_label_set_text(GTK_LABEL(slider_readout_), fmt("Output buffer: {} burst{} · {} ms", bursts, bursts == 1 ? "" : "s", ms).c_str());
    GtkStyleContext* style = gtk_widget_get_style_context(slider_note_);
    if (bursts < floor_bursts_) {
        gtk_label_set_text(GTK_LABEL(slider_note_), fmt("Below the device floor of {} — dropouts likely", floor_bursts_).c_str());
        gtk_style_context_add_class(style, "error");
    } else {
        gtk_label_set_text(GTK_LABEL(slider_note_), fmt("Device floor: {} bursts{}", floor_bursts_, floor_source_.empty() ? "" : " · " + floor_source_).c_str());
        gtk_style_context_remove_class(style, "error");
    }
}

void StatusWindow::append_log(const std::string& line) {
    char stamp[16];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::string text = fmt("{}  {}\n", stamp, line);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buffer_, &end);
    gtk_text_buffer_insert(log_buffer_, &end, text.c_str(), -1);
    GtkTextMark* mark = gtk_text_buffer_get_insert(log_buffer_);
    gtk_text_buffer_get_end_iter(log_buffer_, &end);
    gtk_text_buffer_place_cursor(log_buffer_, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_view_), mark);
}

void StatusWindow::show() {
    gtk_window_present(GTK_WINDOW(window_));
    if (live_ && meter_timer_ == 0) meter_timer_ = g_timeout_add(33, meter_tick, this);
}

}  // namespace ac3spdif
