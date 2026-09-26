// Service mode: a systemd user unit that runs the CLI at login, and an XDG
// autostart entry that opens the app at login. Two different things, easy to
// confuse, so they are kept apart here as they are in the menu.
//
// Both are files the host's systemd and desktop read, and both work from
// inside a Flatpak: the Exec lines then go through `flatpak run`, the files go
// to the host's config directory, which the sandbox exposes at its host path,
// and systemd is driven over its D-Bus API rather than by spawning systemctl.
#pragma once

#include <string>
#include <vector>

namespace ac3spdif::service {

std::string unit_path();
bool is_installed();
bool is_active();
// Writes the unit with the given CLI arguments, reloads, enables and starts it.
void install(const std::vector<std::string>& arguments);
void uninstall();
// The ac3spdif binary the service should run: a sibling of this executable
// when there is one, else whatever PATH finds.
std::string cli_path();

std::string autostart_path();
bool autostart_enabled();
void set_autostart(bool enabled);

}  // namespace ac3spdif::service
