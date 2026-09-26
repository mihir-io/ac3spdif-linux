#include "app/service.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "engine/config.h"
#include "engine/version.h"

namespace ac3spdif::service {

namespace {

constexpr const char* kUnit = "ac3spdif.service";

// --- host or sandbox ---------------------------------------------------------

// The application id when running inside a Flatpak, else empty. Every sandbox
// has a /.flatpak-info naming the application.
const std::string& flatpak_id() {
    static const std::string id = [] {
        std::string result;
        if (!g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) return result;
        GKeyFile* keyfile = g_key_file_new();
        if (g_key_file_load_from_file(keyfile, "/.flatpak-info", G_KEY_FILE_NONE, nullptr)) {
            gchar* name = g_key_file_get_string(keyfile, "Application", "name", nullptr);
            if (name) result = name;
            g_free(name);
        }
        g_key_file_free(keyfile);
        return result;
    }();
    return id;
}

// The config directory the host's systemd and desktop read. Inside a Flatpak
// XDG_CONFIG_HOME points at the app's private directory, so the host's is
// taken from the variable Flatpak forwards, or ~/.config failing that; the
// manifest exposes its systemd and autostart subdirectories at that path.
std::string host_config_dir() {
    if (flatpak_id().empty()) return g_get_user_config_dir();
    const char* host = std::getenv("HOST_XDG_CONFIG_HOME");
    if (host && *host) return host;
    return std::string(g_get_home_dir()) + "/.config";
}

std::string self_path() {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    return n > 0 ? std::string(self, static_cast<size_t>(n)) : std::string();
}

std::string quote(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// An Exec line the host can run for one of this package's binaries.
std::string host_exec(const std::string& binary, const std::vector<std::string>& arguments) {
    std::string exec;
    if (!flatpak_id().empty()) {
        exec = "flatpak run --command=" + binary + " " + flatpak_id();
    } else {
        std::string path = binary == "ac3spdif" ? cli_path() : self_path();
        if (path.empty()) throw Failure("cannot find the " + binary + " binary to run at login");
        exec = quote(path);
    }
    for (const auto& argument : arguments) exec += " " + quote(argument);
    return exec;
}

void write_file(const std::string& path, const std::string& contents) {
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    std::ofstream file(path);
    if (!file) throw Failure("cannot write " + path);
    file << contents;
}

// --- systemd's user manager --------------------------------------------------

// Driven over D-Bus on the session bus, which is where `systemd --user`
// answers on every current distribution and the only route out of a Flatpak.
class Manager {
public:
    Manager() {
        GError* error = nullptr;
        bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!bus_) {
            std::string message = error->message;
            g_error_free(error);
            throw Failure("session bus: " + message);
        }
    }
    ~Manager() { if (bus_) g_object_unref(bus_); }
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    GVariant* call(const char* method, GVariant* parameters) {
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            bus_, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
            method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 15000, nullptr, &error);
        if (!result) {
            std::string message = error->message;
            g_error_free(error);
            throw Failure(std::string("systemd ") + method + ": " + message);
        }
        return result;
    }
    void call_void(const char* method, GVariant* parameters) { g_variant_unref(call(method, parameters)); }

    static GVariant* unit_names() {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&builder, "s", kUnit);
        return g_variant_builder_end(&builder);
    }

    // "active", "inactive", "failed"... or empty when systemd has never seen the unit.
    std::string active_state() {
        GVariant* result = call("ListUnitsByNames", g_variant_new("(@as)", unit_names()));
        GVariantIter* units = nullptr;
        g_variant_get(result, "(a(ssssssouso))", &units);
        std::string state;
        const gchar *name, *description, *load, *active, *sub, *following, *path, *job_type, *job_path;
        guint32 job_id;
        while (g_variant_iter_next(units, "(&s&s&s&s&s&s&ou&s&o)", &name, &description, &load, &active, &sub,
                                   &following, &path, &job_id, &job_type, &job_path)) {
            if (g_strcmp0(name, kUnit) == 0 && g_strcmp0(load, "not-found") != 0) state = active;
        }
        g_variant_iter_free(units);
        g_variant_unref(result);
        return state;
    }

private:
    GDBusConnection* bus_ = nullptr;
};

// systemctl, for a host whose session bus does not carry the manager. Never
// reached inside a Flatpak, which has no systemctl to run.
int systemctl(const std::vector<std::string>& arguments, std::string* output = nullptr) {
    std::vector<std::string> strings = {"systemctl", "--user"};
    strings.insert(strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (const auto& s : strings) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    gchar* out = nullptr;
    gchar* err = nullptr;
    gint status = 0;
    GError* error = nullptr;
    if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err, &status, &error)) {
        std::string message = error ? error->message : "spawn failed";
        g_clear_error(&error);
        throw Failure("systemctl: " + message);
    }
    if (output) *output = std::string(out ? out : "") + (err ? err : "");
    g_free(out);
    g_free(err);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

std::string unit_path() { return host_config_dir() + "/systemd/user/" + kUnit; }

bool is_installed() { return g_file_test(unit_path().c_str(), G_FILE_TEST_EXISTS); }

bool is_active() {
    try {
        Manager manager;
        std::string state = manager.active_state();
        return state == "active" || state == "activating" || state == "reloading";
    } catch (const Failure&) {
        if (!flatpak_id().empty()) return false;
    }
    try {
        return systemctl({"is-active", "--quiet", kUnit}) == 0;
    } catch (const Failure&) {
        return false;
    }
}

std::string cli_path() {
    std::string self = self_path();
    if (!self.empty()) {
        gchar* dir = g_path_get_dirname(self.c_str());
        std::string sibling = std::string(dir) + "/ac3spdif";
        g_free(dir);
        if (g_file_test(sibling.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return sibling;
    }
    gchar* found = g_find_program_in_path("ac3spdif");
    std::string path = found ? found : "";
    g_free(found);
    return path;
}

void install(const std::vector<std::string>& arguments) {
    std::string exec = host_exec("ac3spdif", arguments);
    write_file(unit_path(),
               "[Unit]\n"
               "Description=AC3SPDIF surround bitstream encoder\n"
               "After=pipewire.service wireplumber.service pipewire-pulse.service\n"
               "Wants=pipewire.service\n\n"
               "[Service]\n"
               "ExecStart=" + exec + "\n"
               // Audio devices settle a few seconds after login; a tight respawn
               // loop on failure helps nobody.
               "Restart=on-failure\n"
               "RestartSec=15\n\n"
               "[Install]\n"
               "WantedBy=default.target\n");
    try {
        Manager manager;
        manager.call_void("Reload", nullptr);
        manager.call_void("EnableUnitFiles", g_variant_new("(@asbb)", Manager::unit_names(), FALSE, TRUE));
        manager.call_void("Reload", nullptr);
        manager.call_void("StartUnit", g_variant_new("(ss)", kUnit, "replace"));
        return;
    } catch (const Failure&) {
        if (!flatpak_id().empty()) throw;
    }
    std::string output;
    if (systemctl({"daemon-reload"}) != 0 || systemctl({"enable", "--now", kUnit}, &output) != 0)
        throw Failure("systemctl enable failed: " + output);
}

void uninstall() {
    try {
        Manager manager;
        // Neither matters when the unit was never loaded or enabled.
        try { manager.call_void("StopUnit", g_variant_new("(ss)", kUnit, "replace")); } catch (const Failure&) {}
        try { manager.call_void("DisableUnitFiles", g_variant_new("(@asb)", Manager::unit_names(), FALSE)); } catch (const Failure&) {}
        g_unlink(unit_path().c_str());
        manager.call_void("Reload", nullptr);
        return;
    } catch (const Failure&) {
        g_unlink(unit_path().c_str());
        if (!flatpak_id().empty()) throw;
    }
    systemctl({"disable", "--now", kUnit});
    systemctl({"daemon-reload"});
}

std::string autostart_path() { return host_config_dir() + "/autostart/" + AC3SPDIF_APP_ID + ".desktop"; }

namespace {
// Where releases before the reverse-DNS id wrote the entry.
std::string legacy_autostart_path() { return host_config_dir() + "/autostart/ac3spdif.desktop"; }
}  // namespace

bool autostart_enabled() {
    return g_file_test(autostart_path().c_str(), G_FILE_TEST_EXISTS) ||
           g_file_test(legacy_autostart_path().c_str(), G_FILE_TEST_EXISTS);
}

void set_autostart(bool enabled) {
    g_unlink(legacy_autostart_path().c_str());
    if (!enabled) {
        g_unlink(autostart_path().c_str());
        return;
    }
    write_file(autostart_path(),
               "[Desktop Entry]\nType=Application\nName=AC3SPDIF\nExec=" + host_exec("ac3spdif-app", {}) +
               "\nIcon=" AC3SPDIF_APP_ID "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n");
}

}  // namespace ac3spdif::service
