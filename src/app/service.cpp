#include "app/service.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "engine/config.h"

namespace ac3spdif::service {

namespace {

int run(const std::vector<std::string>& argv_strings, std::string* output = nullptr) {
    std::vector<char*> argv;
    for (const auto& s : argv_strings) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    gchar* out = nullptr;
    gchar* err = nullptr;
    gint status = 0;
    GError* error = nullptr;
    if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err, &status, &error)) {
        std::string message = error ? error->message : "spawn failed";
        g_clear_error(&error);
        throw Failure(argv_strings.front() + ": " + message);
    }
    if (output) *output = std::string(out ? out : "") + (err ? err : "");
    g_free(out);
    g_free(err);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string quote(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

std::string unit_path() {
    return std::string(g_get_user_config_dir()) + "/systemd/user/ac3spdif.service";
}

bool is_installed() { return g_file_test(unit_path().c_str(), G_FILE_TEST_EXISTS); }

bool is_active() {
    try {
        return run({"systemctl", "--user", "is-active", "--quiet", "ac3spdif.service"}) == 0;
    } catch (const Failure&) {
        return false;
    }
}

std::string cli_path() {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        gchar* dir = g_path_get_dirname(self);
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
    std::string cli = cli_path();
    if (cli.empty()) throw Failure("cannot find the ac3spdif command-line binary to run at login");
    std::string exec = quote(cli);
    for (const auto& a : arguments) exec += " " + quote(a);
    std::string path = unit_path();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    std::ofstream unit(path);
    if (!unit) throw Failure("cannot write " + path);
    unit << "[Unit]\n"
            "Description=AC3SPDIF surround bitstream encoder\n"
            "After=pipewire.service wireplumber.service pipewire-pulse.service\n"
            "Wants=pipewire.service\n\n"
            "[Service]\n"
            "ExecStart=" << exec << "\n"
            // Audio devices settle a few seconds after login; a tight respawn
            // loop on failure helps nobody.
            "Restart=on-failure\n"
            "RestartSec=15\n\n"
            "[Install]\n"
            "WantedBy=default.target\n";
    unit.close();
    std::string output;
    run({"systemctl", "--user", "daemon-reload"});
    if (run({"systemctl", "--user", "enable", "--now", "ac3spdif.service"}, &output) != 0)
        throw Failure("systemctl enable failed: " + output);
}

void uninstall() {
    std::string output;
    run({"systemctl", "--user", "disable", "--now", "ac3spdif.service"}, &output);
    g_unlink(unit_path().c_str());
    run({"systemctl", "--user", "daemon-reload"});
}

std::string autostart_path() {
    return std::string(g_get_user_config_dir()) + "/autostart/ac3spdif.desktop";
}

bool autostart_enabled() { return g_file_test(autostart_path().c_str(), G_FILE_TEST_EXISTS); }

void set_autostart(bool enabled) {
    std::string path = autostart_path();
    if (!enabled) {
        g_unlink(path.c_str());
        return;
    }
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    std::string exe = n > 0 ? std::string(self, static_cast<size_t>(n)) : "ac3spdif-app";
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    std::ofstream file(path);
    if (!file) throw Failure("cannot write " + path);
    file << "[Desktop Entry]\nType=Application\nName=AC3SPDIF\nExec=" << quote(exe)
         << "\nIcon=ac3spdif\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
}

}  // namespace ac3spdif::service
