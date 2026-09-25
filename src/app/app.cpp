#include "app/app.h"

#if defined(HAVE_AYATANA_APPINDICATOR)
#include <libayatana-appindicator/app-indicator.h>
#elif defined(HAVE_APPINDICATOR)
#include <libappindicator/app-indicator.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

#include "app/icons.h"
#include "app/service.h"
#include "engine/alsa_devices.h"
#include "engine/log.h"
#include "engine/version.h"

namespace ac3spdif {

namespace {

// While the menu is being synchronised to the settings, check items are set
// programmatically; GTK emits activate for those too, and they must not act.
bool g_syncing = false;

// Menu items carry a std::function; dbusmenu only knows labels, checks and
// separators, which is all a tray menu needs.
void on_item_activate(GtkMenuItem* item, gpointer) {
    if (g_syncing) return;
    auto* fn = static_cast<std::function<void()>*>(g_object_get_data(G_OBJECT(item), "ac3spdif-fn"));
    // Run it from the main loop, not from inside the signal, so an action can
    // never destroy the item whose handler is still executing.
    if (fn) {
        auto* copy = new std::function<void()>(*fn);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
            (*static_cast<std::function<void()>*>(p))();
            return G_SOURCE_REMOVE;
        }, copy, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
    }
}

void attach(GtkWidget* item, std::function<void()> fn) {
    g_object_set_data_full(G_OBJECT(item), "ac3spdif-fn", new std::function<void()>(std::move(fn)),
                           [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
    g_signal_connect(item, "activate", G_CALLBACK(on_item_activate), nullptr);
}

GtkWidget* add_item(GtkWidget* menu, const std::string& label, std::function<void()> fn, bool sensitive = true) {
    GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
    gtk_widget_set_sensitive(item, sensitive);
    if (fn) attach(item, std::move(fn));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* add_check(GtkWidget* menu, const std::string& label, bool checked, bool radio,
                     std::function<void()> fn, bool sensitive = true) {
    GtkWidget* item = gtk_check_menu_item_new_with_label(label.c_str());
    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), radio);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
    gtk_widget_set_sensitive(item, sensitive);
    if (fn) attach(item, std::move(fn));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* add_separator(GtkWidget* menu) {
    GtkWidget* item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* add_submenu(GtkWidget* menu, const std::string& label) {
    GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
    GtkWidget* sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return sub;
}

// In-place property updates. Each one is a D-Bus property change to the
// panel, so only send what actually changed.
void set_label(GtkWidget* item, const std::string& text) {
    const char* now = gtk_menu_item_get_label(GTK_MENU_ITEM(item));
    if (!now || text != now) gtk_menu_item_set_label(GTK_MENU_ITEM(item), text.c_str());
}

void set_visible(GtkWidget* item, bool visible) {
    if (gtk_widget_get_visible(item) != visible) gtk_widget_set_visible(item, visible);
}

void set_sensitive(GtkWidget* item, bool sensitive) {
    if (gtk_widget_get_sensitive(item) != sensitive) gtk_widget_set_sensitive(item, sensitive);
}

void set_active(GtkWidget* item, bool active) {
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)) != active)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
}

template <typename F>
void on_main_thread(F fn) {
    auto* boxed = new std::function<void()>(std::move(fn));
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        (*static_cast<std::function<void()>*>(p))();
        return G_SOURCE_REMOVE;
    }, boxed, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}

// A signed number in a fixed number of cells, padded on the left with FIGURE
// SPACE (the width of a digit in any font with tabular figures) and using the
// real MINUS SIGN, which is as wide as a plus.
std::string fixed_width(double value, int digits, bool sign) {
    std::string number = fmt("{:.0f}", std::fabs(value) < 0.5 ? 0.0 : std::fabs(value));
    if (static_cast<int>(number.size()) > digits) number = std::string(static_cast<size_t>(digits), '9');
    std::string out = sign ? (value < -0.5 ? "−" : "+") : "";
    for (int i = static_cast<int>(number.size()); i < digits; ++i) out = " " + out;
    return out + number;
}

constexpr float kFloorDb = -60.0f;
float to_db(float linear) { return linear <= 0 ? kFloorDb : std::max(kFloorDb, 20.0f * std::log10(linear)); }
float from_db(float db) { return db <= kFloorDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

// PipeWire appends ".N" when a node is recreated while an old one lingers.
std::string base_name(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return name;
    for (size_t i = dot + 1; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    return name.substr(0, dot);
}

bool same_device(const std::string& a, const std::string& b) {
    return a == b || (!a.empty() && !b.empty() && base_name(a) == base_name(b));
}

// The names the device submenus were built from. Our own virtual sink is
// left out: it comes and goes with every run, and that is not a change.
std::vector<std::string> device_names(const std::optional<Snapshot>& snap) {
    std::vector<std::string> names;
    if (snap) {
        for (const NodeInfo* n : snap->sinks()) if (n->name != kDefaultVirtualSink) names.push_back(n->name);
        for (const NodeInfo* n : snap->sources()) names.push_back(n->name);
    }
    for (const auto& pcm : alsa_digital_pcms()) names.push_back("alsa:" + pcm.name);
    return names;
}

}  // namespace

App::App(GtkApplication* application) : application_(application) {
    g_application_hold(G_APPLICATION(application_));
    settings_.load();
    // Write straight back: a file from an older version is rewritten in the
    // current layout, and a first run leaves a complete file to edit.
    settings_.save();
    window_ = std::make_unique<StatusWindow>(application_);
    window_->on_toggle = [this] { toggle_engine(); };
    window_->on_latency_changed = [this](int bursts) { latency_changed(bursts); };
    window_->live_levels = [this] { return engine_ ? engine_->live_input_levels() : std::vector<float>{}; };
    icon_dir_ = icon_directory();
    build_indicator();
    build_menu();
#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (indicator_) {
        app_indicator_set_title(indicator_, "AC3SPDIF");
        app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    }
#endif
    update_icon(EngineState::Idle);
    std::string source;
    window_->configure_latency(settings_.prebuffer_bursts(), latency_floor(&source), source, settings_.sample_rate, settings_.codec);
    refresh_timer_ = g_timeout_add(500, [](gpointer data) -> gboolean {
        auto* self = static_cast<App*>(data);
        EngineStatus status = self->engine_ ? self->engine_->status() : EngineStatus{};
        self->last_status_ = status;
        self->window_->update(status, self->settings_.prebuffer_bursts(), self->settings_.channels, self->settings_.sample_rate);
        if (status.state != self->synced_state_) self->sync_menu();
        self->refresh_stats();
        self->set_meter_timer(status.state == EngineState::Running);
        // Devices come and go; look every half minute while idle, and only
        // rebuild the menu when the list really changed.
        if (!state_is_active(status.state)) {
            gint64 now = g_get_monotonic_time();
            if (now - self->last_device_poll_ > 30 * G_USEC_PER_SEC) {
                self->last_device_poll_ = now;
                self->maybe_rebuild_for_devices();
            }
        }
        return G_SOURCE_CONTINUE;
    }, this);
    last_device_poll_ = g_get_monotonic_time();
    if (!indicator_) window_->show();
    if (settings_.auto_start) start_engine();
}

App::~App() {
    if (refresh_timer_) g_source_remove(refresh_timer_);
    if (meter_timer_) g_source_remove(meter_timer_);
    if (latency_commit_timer_) g_source_remove(latency_commit_timer_);
}

void App::build_indicator() {
#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (icon_dir_.empty()) {
        indicator_ = app_indicator_new("ac3spdif", "audio-card", APP_INDICATOR_CATEGORY_HARDWARE);
        append_log("icon files not found; using a stock icon in the tray");
    } else {
        indicator_ = app_indicator_new_with_path("ac3spdif", "ac3spdif-idle-symbolic",
                                                 APP_INDICATOR_CATEGORY_HARDWARE, icon_dir_.c_str());
    }
#else
    append_log("built without libappindicator; there is no tray icon, use this window");
#endif
}

void App::update_icon(EngineState state) {
#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (!indicator_ || icon_dir_.empty()) return;
    // One mark in four conditions: the socket is always there, the ferrule
    // is hollow when nothing flows, solid while starting or stopping, red
    // while the bitstream is live, and struck through after a failure.
    const char* icon = "ac3spdif-idle-symbolic";
    switch (state) {
        case EngineState::Running: icon = "ac3spdif-live"; break;
        case EngineState::Starting:
        case EngineState::Stopping: icon = "ac3spdif-busy-symbolic"; break;
        case EngineState::Failed: icon = "ac3spdif-failed-symbolic"; break;
        case EngineState::Idle: break;
    }
    app_indicator_set_icon_full(indicator_, icon, fmt("ac3spdif {}", state_label(state)).c_str());
#else
    (void)state;
#endif
}

std::optional<Snapshot> App::devices() {
    try {
        if (!session_) session_ = std::make_unique<PwSession>();
        return session_->snapshot();
    } catch (const Failure& failure) {
        session_.reset();
        append_log(std::string("PipeWire: ") + failure.what());
        return std::nullopt;
    }
}

int App::latency_floor(std::string* source) const {
    // The real request size is only known once the stream runs; before that
    // the requested buffer is the best estimate.
    int frames = settings_.device_buffer_frames;
    if (last_status_.state == EngineState::Running && last_status_.latency.device_frames > 0)
        frames = std::max(frames, last_status_.latency.device_frames);
    if (source) *source = fmt("{}-frame requests, {}", frames, codec_short_name(settings_.codec));
    return latency_advice::minimum_bursts(frames, settings_.codec, settings_.sample_rate);
}

// --- the menu ----------------------------------------------------------------

void App::build_menu() {
    snapshot_ = devices();
    device_names_ = device_names(snapshot_);
    MenuItems m;
    GtkWidget* menu = gtk_menu_new();

    m.header = add_item(menu, state_label(EngineState::Idle), nullptr, false);
    m.stats = add_item(menu, "", nullptr, false);
    add_separator(menu);
    m.start_stop = add_item(menu, "Start", [this] { toggle_engine(); });
    m.status = add_item(menu, "Status Window…", [this] { show_status(); });

    // Channel activity, the way the macOS menu shows it behind Option: a bar
    // of block glyphs per channel on the dB scale, updated live. The rows
    // exist always and are shown only while streaming; the section header
    // carries invisible trailing figure spaces so it is always the widest row
    // of the menu, which keeps the menu's width constant as the bars change.
    m.meters_separator = add_separator(menu);
    m.meters_header = add_item(menu, "Channel Activity — levels into the encoder, dBFS"
                                     "          ", nullptr, false);
    for (int i = 0; i < 8; ++i) m.meter_rows.push_back(add_item(menu, "", [this] { show_status(); }));
    GtkWidget* meters = add_submenu(menu, "Meters");
    m.meters_in_menu = add_check(meters, "In this menu while streaming", settings_.meters_in_menu, false,
                                 [this] { settings_.meters_in_menu = !settings_.meters_in_menu; settings_.save(); sync_menu(); });
    m.meters_in_panel = add_check(meters, "Beside the panel icon", settings_.meters_in_panel, false,
                                  [this] { settings_.meters_in_panel = !settings_.meters_in_panel; settings_.save(); sync_menu(); });
    add_separator(meters);
    add_item(meters, "Full meters with peak marks: Status Window (or middle-click the icon)", nullptr, false);
    add_separator(menu);

    GtkWidget* capture = add_submenu(menu, "Capture Source");
    m.capture.emplace_back("auto", add_check(capture, fmt("Automatic (virtual sink \"{}\")", kDefaultVirtualSink), false, true,
                                             [this] { settings_.input = "auto"; setting_changed(); }));
    if (snapshot_) {
        add_separator(capture);
        for (const NodeInfo* sink : snapshot_->sinks()) {
            if (sink->name == kDefaultVirtualSink) continue;
            std::string value = sink->name;
            m.capture.emplace_back(value, add_check(capture, sink->display_name() + "  (monitor)", false, true,
                                                    [this, value] { settings_.input = value; setting_changed(); }));
        }
        for (const NodeInfo* source : snapshot_->sources()) {
            std::string value = source->name;
            m.capture.emplace_back(value, add_check(capture, source->display_name(), false, true,
                                                    [this, value] { settings_.input = value; setting_changed(); }));
        }
    }

    GtkWidget* output = add_submenu(menu, "Output Device");
    m.output.push_back({"", add_check(output, "Automatic (best passthrough device)", false, true,
                                      [this] { settings_.output.clear(); setting_changed(); }), std::nullopt});
    if (snapshot_) {
        add_separator(output);
        for (const NodeInfo* sink : snapshot_->sinks()) {
            if (sink->is_virtual()) continue;
            std::string value = sink->name;
            m.output.push_back({value, add_check(output, sink->display_name(), false, true,
                                                 [this, value] { settings_.output = value; setting_changed(); }), *sink});
        }
    }
    auto pcms = alsa_digital_pcms();
    if (!pcms.empty()) {
        add_separator(output);
        add_item(output, "Direct ALSA, exclusive (takes the card from the desktop)", nullptr, false);
        for (const auto& pcm : pcms) {
            std::string value = "alsa:" + pcm.name;
            m.output.push_back({value, add_check(output, fmt("{}  ({})", pcm.description, pcm.name), false, true,
                                                 [this, value] { settings_.output = value; setting_changed(); }), std::nullopt});
        }
    }

    GtkWidget* channels = add_submenu(menu, "Channels");
    for (auto [count, label] : {std::pair{2, "Stereo (2.0)"}, std::pair{6, "Surround (5.1)"}, std::pair{8, "8-channel source → 5.1"}}) {
        int value = count;
        m.channels[count] = add_check(channels, label, false, true, [this, value] { settings_.channels = value; setting_changed(); });
    }

    GtkWidget* codec = add_submenu(menu, "Codec");
    for (Codec c : {Codec::AC3, Codec::DTS}) {
        int burst_ms = latency_advice::milliseconds(1, settings_.sample_rate, c);
        m.codec[c] = add_check(codec, fmt("{}  —  {} ms burst", codec_name(c), burst_ms), false, true,
                               [this, c] { choose_codec(c); });
    }
    add_separator(codec);
    add_item(codec, "DTS frames are a third the length, so latency drops with them", nullptr, false);

    // Every bitrate of both codecs is an item; the ones for the other codec
    // are hidden. The sets do not overlap, so nothing is ambiguous.
    GtkWidget* bitrate = add_submenu(menu, "Bitrate");
    m.bitrate_auto = add_check(bitrate, "Automatic", false, true, [this] { settings_.set_bitrate(""); setting_changed(); });
    add_separator(bitrate);
    for (Codec c : {Codec::AC3, Codec::DTS}) {
        for (const auto& rate : supported_bitrates(c)) {
            std::string value = rate;
            m.bitrates.push_back({{c, rate}, add_check(bitrate, rate, false, true,
                                                       [this, value] { settings_.set_bitrate(value); setting_changed(); })});
        }
    }

    GtkWidget* transport = add_submenu(menu, "Transport");
    for (Transport t : {Transport::Passthrough, Transport::Pcm, Transport::Alsa}) {
        m.transport[t] = add_check(transport, transport_label(t), false, true, [this, t] {
            settings_.transport = t;
            if (t == Transport::Alsa && settings_.output.rfind("alsa:", 0) != 0) {
                settings_.output.clear();
                append_log("direct ALSA selected; the first digital ALSA output is used unless one is chosen");
            }
            setting_changed();
        });
    }

    GtkWidget* latency = add_submenu(menu, "Latency");
    for (LatencyProfile p : kAllProfiles) {
        int bursts = profile_bursts(p);
        m.latency[bursts] = add_check(latency, profile_name(p), false, true,
                                      [this, bursts] { settings_.set_prebuffer_bursts(bursts); setting_changed(); });
    }
    add_separator(latency);
    m.latency_custom = add_item(latency, "Custom…", [this] { show_status(); });
    add_item(latency, "Ring buffer only; add the frame, capture and device terms", nullptr, false);

    m.self_test = add_check(menu, "Self-Test Tones", settings_.self_test, false,
                            [this] { settings_.self_test = !settings_.self_test; setting_changed(); });
    m.route = add_check(menu, "Route System Audio to the Encoder", settings_.set_default_sink, false,
                        [this] { settings_.set_default_sink = !settings_.set_default_sink; setting_changed(); });
    add_separator(menu);
    m.service = add_check(menu, "Run as Background Service", service::is_installed(), false, [this] { toggle_service(); });
    m.auto_start = add_check(menu, "Start Streaming When App Opens", settings_.auto_start, false,
                             [this] { settings_.auto_start = !settings_.auto_start; settings_.save(); sync_menu(); });
    m.login = add_check(menu, "Launch App at Login", service::autostart_enabled(), false, [this] {
        try { service::set_autostart(!service::autostart_enabled()); } catch (const Failure& f) { present_failure(f.what()); }
        sync_menu();
    });
    add_separator(menu);
    add_item(menu, fmt("Version {}", AC3SPDIF_VERSION), nullptr, false);
    add_item(menu, "Quit AC3SPDIF", [this] { quit(); });
    gtk_widget_show_all(menu);

#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (indicator_) {
        // Drop the old middle-click target before its menu goes away, or
        // libappindicator trips over the orphaned widget.
        app_indicator_set_secondary_activate_target(indicator_, nullptr);
        app_indicator_set_menu(indicator_, GTK_MENU(menu));
        // Middle-click on the icon: straight to the full meters.
        app_indicator_set_secondary_activate_target(indicator_, m.status);
    }
#endif
    if (menu_) g_object_unref(menu_);
    menu_ = GTK_WIDGET(g_object_ref_sink(menu));
    items_ = m;
    sync_menu();
}

// Bring every item into line with the settings and the engine state, by
// changing properties on the items that exist.
void App::sync_menu() {
    if (!items_.header) return;
    struct Guard { Guard() { g_syncing = true; } ~Guard() { g_syncing = false; } } guard;
    MenuItems& m = items_;
    EngineState state = engine_ ? engine_->state() : EngineState::Idle;
    synced_state_ = state;
    bool running = state == EngineState::Running;

    std::string header;
    if (running) {
        // Some HDMI descriptions run to ninety characters; the status window
        // has the full name, the menu gets one that fits.
        std::string name = last_status_.output_name;
        if (name.size() > 48) name = name.substr(0, 30) + "…" + name.substr(name.size() - 16);
        header = fmt("Streaming {} to {}", codec_short_name(settings_.codec), name);
    } else if (state == EngineState::Failed) {
        header = "Failed — " + last_status_.failure;
    } else {
        header = state_label(state);
    }
    set_label(m.header, header);
    set_visible(m.stats, running);
    set_label(m.start_stop, state_is_active(state) ? "Stop" : "Start");
    set_sensitive(m.start_stop, state != EngineState::Starting && state != EngineState::Stopping);

    bool show_meters = running && settings_.meters_in_menu;
    set_visible(m.meters_separator, show_meters);
    set_visible(m.meters_header, show_meters);
    for (size_t i = 0; i < m.meter_rows.size(); ++i)
        set_visible(m.meter_rows[i], show_meters && static_cast<int>(i) < settings_.channels);
    set_active(m.meters_in_menu, settings_.meters_in_menu);
    set_active(m.meters_in_panel, settings_.meters_in_panel);

    std::string input = settings_.input.empty() ? "auto" : settings_.input;
    for (auto& [value, item] : m.capture) set_active(item, value == "auto" ? input == "auto" : same_device(value, input));
    for (auto& entry : m.output) {
        set_active(entry.item, entry.value.empty() ? settings_.output.empty() : same_device(entry.value, settings_.output));
        if (entry.node && snapshot_)
            set_label(entry.item, fmt("{}  [{}]", entry.node->display_name(), capability_label(*snapshot_, *entry.node, settings_.codec)));
    }
    for (auto& [count, item] : m.channels) set_active(item, count == settings_.channels);
    for (auto& [c, item] : m.codec) set_active(item, c == settings_.codec);

    bool automatic = settings_.bitrate().empty() || !accepts_bitrate(settings_.codec, settings_.bitrate());
    set_label(m.bitrate_auto, fmt("Automatic ({})", default_bitrate(settings_.codec)));
    set_active(m.bitrate_auto, automatic);
    for (auto& [key, item] : m.bitrates) {
        set_visible(item, key.first == settings_.codec);
        set_active(item, !automatic && key.first == settings_.codec && key.second == settings_.bitrate());
    }
    for (auto& [t, item] : m.transport) set_active(item, t == settings_.transport);

    std::string floor_source;
    int floor = latency_floor(&floor_source);
    for (auto& [bursts, item] : m.latency) {
        int ms = latency_advice::milliseconds(bursts, settings_.sample_rate, settings_.codec);
        std::string suffix = bursts == profile_bursts(LatencyProfile::Safe) ? "  (default)" : bursts < floor ? "  (below floor)" : "";
        set_label(item, fmt("{} — {} bursts, {} ms{}", profile_name(*profile_matching(bursts)), bursts, ms, suffix));
        set_active(item, bursts == settings_.prebuffer_bursts());
    }
    set_label(m.latency_custom, fmt("Custom… (now {} bursts; device floor {})", settings_.prebuffer_bursts(), floor));

    set_active(m.self_test, settings_.self_test);
    set_active(m.route, settings_.set_default_sink);
    set_active(m.service, service::is_installed());
    set_active(m.auto_start, settings_.auto_start);
    set_active(m.login, service::autostart_enabled());
}

// The live figures on their own short row, each in fixed-width cells with
// the number that changes every refresh placed last, so neither a changing
// digit count nor a long device name can resize the menu.
void App::refresh_stats() {
    if (!items_.stats || last_status_.state != EngineState::Running) return;
    int target_digits = last_status_.target_bursts >= 10 ? 2 : 1;
    std::string stats = fmt("buffer {}/{} · drift {} ppm",
                            fixed_width(last_status_.buffer_bursts, target_digits, false),
                            last_status_.target_bursts, fixed_width(last_status_.drift_ppm, 4, true));
    if (last_status_.underrun_bytes > 0)
        stats += fmt(" · {} underruns", fixed_width(static_cast<double>(last_status_.underrun_bytes / 1024) + 1, 4, false) + " KB");
    set_label(items_.stats, stats);
}

void App::maybe_rebuild_for_devices() {
    std::optional<Snapshot> snap = devices();
    if (!snap) return;
    std::vector<std::string> names = device_names(snap);
    if (names == device_names_) {
        snapshot_ = snap;
        return;
    }
    append_log("audio devices changed; menus updated");
    build_menu();
}

// --- meters --------------------------------------------------------------------

// One bar per channel on the dB scale: twenty segments of 3 dB, a peak mark
// that falls back over about a second, and the reading. The bar comes first
// so the bars line up whatever the panel's proportional font does to the
// channel names; every glyph used is one fonts draw at a single width.
std::string App::meter_row(size_t channel, float level, float peak) const {
    constexpr int kSegments = 20;
    auto names = channel_position_names(settings_.channels);
    std::string name = channel < names.size() ? names[channel] : std::to_string(channel + 1);
    int filled = static_cast<int>(std::lround((to_db(level) - kFloorDb) / -kFloorDb * kSegments));
    int mark = static_cast<int>(std::lround((to_db(peak) - kFloorDb) / -kFloorDb * kSegments)) - 1;
    std::string bar;
    for (int i = 0; i < kSegments; ++i) {
        if (i < filled) bar += "█";                     // full block
        else if (i == mark && peak > 0) bar += "▌";     // left half block: the peak
        else bar += "░";                                // light shade: the empty track
    }
    float db = to_db(level);
    // Sign and two digit cells, so the reading keeps its width from -60 to 0.
    std::string reading = db <= kFloorDb ? " −∞" : fixed_width(db, 2, true) + " dB";
    // Red is not available in a text label; a mark near full scale says clip.
    if (db > -1.0f) reading += "  !";
    while (name.size() < 3) name += " ";
    return fmt("{}  {} {}", bar, name, reading);
}

// Beside the icon: one glyph per channel, four shades on the dB scale. The
// eight-step bar ramp the CLI uses would be finer, but fonts draw its glyphs
// at two different widths, and a panel label that changes width ten times a
// second walks the whole panel. The four shades share one width everywhere.
std::string App::panel_meter(const std::vector<float>& levels) const {
    static const char* ramp[] = {"░", "▒", "▓", "█"};
    std::string out;
    for (float level : levels) {
        int step = static_cast<int>(std::lround((to_db(level) - kFloorDb) / -kFloorDb * 3));
        out += ramp[std::clamp(step, 0, 3)];
    }
    return out;
}

void App::set_meter_timer(bool running) {
    if (running && !meter_timer_) {
        last_meter_tick_ = 0;
        meter_timer_ = g_timeout_add(100, [](gpointer data) -> gboolean {
            static_cast<App*>(data)->meter_tick();
            return G_SOURCE_CONTINUE;
        }, this);
    } else if (!running && meter_timer_) {
        g_source_remove(meter_timer_);
        meter_timer_ = 0;
    }
#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (indicator_ && !running && panel_label_shown_) {
        app_indicator_set_label(indicator_, "", "");
        panel_label_shown_ = false;
    }
#endif
}

void App::meter_tick() {
    if (!engine_) return;
    std::vector<float> levels = engine_->live_input_levels();
    size_t wanted = static_cast<size_t>(std::max(settings_.channels, 1));
    levels.resize(wanted, 0.0f);
    if (menu_peaks_.size() != wanted) menu_peaks_ = levels;
    gint64 now = g_get_monotonic_time();
    float elapsed = last_meter_tick_ ? std::min(0.5f, static_cast<float>(now - last_meter_tick_) / 1e6f) : 0.1f;
    last_meter_tick_ = now;
    float fall_db = 24.0f * elapsed;
    for (size_t i = 0; i < wanted; ++i) {
        float decayed = from_db(to_db(menu_peaks_[i]) - fall_db);
        menu_peaks_[i] = std::max(levels[i], std::min(menu_peaks_[i], decayed));
    }
    if (settings_.meters_in_menu) {
        for (size_t i = 0; i < items_.meter_rows.size() && i < wanted; ++i)
            set_label(items_.meter_rows[i], meter_row(i, levels[i], menu_peaks_[i]));
    }
#if defined(HAVE_AYATANA_APPINDICATOR) || defined(HAVE_APPINDICATOR)
    if (indicator_) {
        if (settings_.meters_in_panel) {
            // The guide is the widest the label can get, so the icon does not
            // jitter sideways as the glyphs change.
            std::string guide;
            for (size_t i = 0; i < wanted; ++i) guide += "█";
            app_indicator_set_label(indicator_, panel_meter(levels).c_str(), guide.c_str());
            panel_label_shown_ = true;
        } else if (panel_label_shown_) {
            app_indicator_set_label(indicator_, "", "");
            panel_label_shown_ = false;
        }
    }
#endif
}

// --- engine glue -------------------------------------------------------------

void App::show_status() { window_->show(); }

void App::append_log(const std::string& line) { if (window_) window_->append_log(line); }

void App::start_engine() {
    if (engine_ && state_is_active(engine_->state())) return;
    if (service::is_active()) {
        GtkWidget* dialog = gtk_message_dialog_new(window_->window(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                                   "Service mode is running");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "The background service already holds the output device, so the app cannot also stream to it. Stop the service first?");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL, "Stop Service and Start", GTK_RESPONSE_ACCEPT, nullptr);
        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_ACCEPT) return;
        try { service::uninstall(); } catch (const Failure& f) { present_failure(f.what()); return; }
        sync_menu();
    }
    engine_ = std::make_unique<Engine>(settings_.to_config());
    engine_->on_state = [this](EngineState s) { on_main_thread([this, s] { handle_state(s); }); };
    engine_->on_log = [this](const std::string& line) { on_main_thread([this, line] { append_log(line); }); };
    engine_->start();
}

void App::stop_engine() { if (engine_) engine_->stop(); }

void App::toggle_engine() {
    if (engine_ && state_is_active(engine_->state())) stop_engine();
    else start_engine();
}

void App::handle_state(EngineState state) {
    update_icon(state);
    switch (state) {
        case EngineState::Running: append_log("Streaming."); break;
        case EngineState::Idle: append_log("Stopped; default sink and output restored."); break;
        case EngineState::Failed: {
            std::string message = engine_ ? engine_->status().failure : "unknown";
            append_log("Failed: " + message);
            if (!restart_pending_ && !quitting_) present_failure(message);
            break;
        }
        default: break;
    }
    if ((state == EngineState::Idle || state == EngineState::Failed) && restart_pending_ && !quitting_) {
        restart_pending_ = false;
        start_engine();
    }
    last_status_ = engine_ ? engine_->status() : EngineStatus{};
    sync_menu();
    if (!state_is_active(state) && !restart_pending_) maybe_rebuild_for_devices();
}

void App::present_failure(const std::string& message) {
    GtkWidget* dialog = gtk_message_dialog_new(window_->window(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                               "Could not start streaming");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Show Log", 1, "OK", 2, nullptr);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == 1) show_status();
}

// Settings are baked into the Config at start, so a change only takes effect
// on a restart. Doing that is friendlier than leaving the menu showing one
// thing while the stream does another.
void App::setting_changed() {
    settings_.save();
    std::string source;
    window_->configure_latency(settings_.prebuffer_bursts(), latency_floor(&source), source, settings_.sample_rate, settings_.codec);
    restart_if_running();
    sync_menu();
}

void App::choose_codec(Codec codec) {
    settings_.codec = codec;
    // Each codec keeps its own bitrate and output buffer, so nothing carries
    // over. A remembered buffer below this codec's floor is still honoured;
    // the menu marks it and the engine warns, but neither forbids it.
    int floor = latency_floor(nullptr);
    if (settings_.prebuffer_bursts() < floor)
        append_log(fmt("{} output buffer is {} bursts, below the floor of {} for this device: expect dropouts",
                       codec_name(codec), settings_.prebuffer_bursts(), floor));
    setting_changed();
}

void App::restart_if_running() {
    if (!engine_ || !state_is_active(engine_->state())) return;
    append_log("Restarting to apply the new setting…");
    restart_pending_ = true;
    engine_->stop();
}

// Dragging a slider fires continuously; restarting the pipeline on every tick
// would be unusable. Save immediately, restart once things settle.
void App::latency_changed(int bursts) {
    settings_.set_prebuffer_bursts(bursts);
    settings_.save();
    if (latency_commit_timer_) g_source_remove(latency_commit_timer_);
    latency_commit_timer_ = g_timeout_add(600, [](gpointer data) -> gboolean {
        auto* self = static_cast<App*>(data);
        self->latency_commit_timer_ = 0;
        self->restart_if_running();
        self->sync_menu();
        return G_SOURCE_REMOVE;
    }, this);
}

void App::toggle_service() {
    try {
        if (service::is_installed()) {
            service::uninstall();
            append_log("Background service removed.");
        } else {
            if (engine_ && state_is_active(engine_->state())) {
                stop_engine();
                append_log("Stopped app streaming so the service can take the device.");
            }
            service::install(settings_.cli_arguments());
            append_log("Background service installed; it also starts at login.");
        }
    } catch (const Failure& f) {
        present_failure(f.what());
    }
    sync_menu();
}

void App::quit() {
    quitting_ = true;
    if (engine_ && state_is_active(engine_->state())) {
        // Never exit while the sink is still switched over and the output
        // claimed: they are restored on the worker thread, so wait for it.
        append_log("Stopping and restoring the devices…");
        engine_->stop();
        engine_->wait_until_stopped(std::chrono::seconds(8));
    }
    engine_.reset();
    g_application_release(G_APPLICATION(application_));
    g_application_quit(G_APPLICATION(application_));
}

}  // namespace ac3spdif
