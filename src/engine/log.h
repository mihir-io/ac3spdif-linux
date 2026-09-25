// A log line is a string handed to whoever is listening: the CLI prints it,
// the app appends it to its status window. Nothing here writes to stderr on
// its own.
#pragma once

#include <format>
#include <functional>
#include <string>

namespace ac3spdif {

using LogSink = std::function<void(const std::string&)>;

template <typename... Args>
std::string fmt(std::format_string<Args...> format, Args&&... args) {
    return std::format(format, std::forward<Args>(args)...);
}

}  // namespace ac3spdif
