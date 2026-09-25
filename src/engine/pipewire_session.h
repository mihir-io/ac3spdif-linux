// The connection to PipeWire, and the registry model built from it.
//
// One thread loop, one core, one registry listener. Every Audio/Sink and
// Audio/Source node and every Audio/Device is bound so its properties and the
// parameters that matter here (the IEC 958 codec list, the formats it offers,
// the routes and their port types) are tracked as they change. Front ends read
// snapshots; the engine uses the same session to create its streams.
//
// Everything that touches a PipeWire object must hold the loop lock. Callbacks
// arrive on the loop thread with the lock already held.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/config.h"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_proxy;
struct spa_hook;
struct spa_pod;

namespace ac3spdif {

struct NodeInfo {
    uint32_t id = 0;
    uint64_t serial = 0;
    std::string name, description, nick, media_class, state;
    std::map<std::string, std::string> props;
    std::vector<std::string> iec958_codecs;      // enabled codecs (Props param)
    std::set<std::string> offered_iec958_codecs; // codecs in EnumFormat
    std::vector<std::string> format_lines;       // one line per EnumFormat entry
    int channels = 0;
    int rate = 0;

    bool is_sink() const { return media_class == "Audio/Sink" || media_class == "Audio/Duplex"; }
    bool is_source() const { return media_class == "Audio/Source" || media_class == "Audio/Source/Virtual" || media_class == "Audio/Duplex"; }
    std::string prop(const std::string& key) const {
        auto it = props.find(key);
        return it == props.end() ? std::string() : it->second;
    }
    std::string api() const { return prop("device.api"); }
    // PipeWire appends ".N" when a node is recreated while an old one lingers,
    // so a saved name is matched with that suffix ignored.
    std::string base_name() const;
    uint32_t device_id() const;
    int profile_device() const;
    bool is_virtual() const { return prop("node.virtual") == "true"; }
    bool has_codec(Codec c) const;
    std::string display_name() const { return description.empty() ? name : description; }
};

struct RouteInfo {
    int index = -1;
    int device = -1;
    std::string name, description, direction, port_type;
    int available = 0;                       // 0 unknown, 1 no, 2 yes
    std::vector<std::string> iec958_codecs;
    std::vector<float> channel_volumes;
    bool mute = false;
    bool has_props = false;
    std::vector<int> profiles;
    std::map<std::string, std::string> info;
};

struct DeviceInfo {
    uint32_t id = 0;
    std::string name, description, api;
    std::map<std::string, std::string> props;
    std::vector<RouteInfo> routes;        // active routes (SPA_PARAM_Route)
    std::vector<RouteInfo> enum_routes;   // every possible route
    std::string prop(const std::string& key) const {
        auto it = props.find(key);
        return it == props.end() ? std::string() : it->second;
    }
};

struct Snapshot {
    std::vector<NodeInfo> nodes;
    std::vector<DeviceInfo> devices;
    std::string default_sink, default_source;
    std::string configured_default_sink;
    std::string server_version;
    std::string server_name;

    const NodeInfo* node_by_id(uint32_t id) const;
    const DeviceInfo* device_by_id(uint32_t id) const;
    // The active output route of a sink node, if its device reports one.
    const RouteInfo* route_for(const NodeInfo& sink) const;
    std::vector<const NodeInfo*> sinks() const;
    std::vector<const NodeInfo*> sources() const;
};

enum class NodeRole { Any, Sink, Source };

class PwSession {
public:
    PwSession();
    ~PwSession();
    PwSession(const PwSession&) = delete;
    PwSession& operator=(const PwSession&) = delete;

    struct Lock {
        explicit Lock(PwSession& s);
        ~Lock();
        PwSession& session;
    };
    Lock lock() { return Lock(*this); }

    pw_core* core() const { return core_; }
    pw_thread_loop* loop() const { return loop_; }

    // Roundtrip to the server. Lock must be held.
    void sync();
    // Wait on the loop condition. Lock must be held.
    void wait();
    void signal();

    // A fresh copy of everything known, after settling the registry.
    Snapshot snapshot();
    std::optional<NodeInfo> find_node(const std::string& query, NodeRole role);
    std::optional<NodeInfo> node_by_id(uint32_t id);

    // A null sink owned by this connection: it disappears when the process
    // does, so a crash cannot leave the desktop routed into nothing.
    NodeInfo create_virtual_sink(const std::string& name, const std::string& description,
                                 int channels, int sample_rate);
    void destroy_virtual_sink();

    // The user's configured default sink, through the "default" metadata.
    std::string configured_default_sink();
    void set_configured_default_sink(const std::string& name);
    void clear_configured_default_sink();

    // Make a sink advertise the given IEC 958 codecs. Goes through the device
    // route so WirePlumber persists it, falling back to the node's own
    // properties. Returns true when the codec is confirmed afterwards.
    bool enable_iec958_codecs(const NodeInfo& sink, const std::vector<std::string>& codecs);
    // Unity gain and unmuted on a sink for the duration of a PCM-carrier run.
    // Returns the previous route props so they can be put back.
    std::optional<RouteInfo> neutralise_sink_levels(const NodeInfo& sink);
    void restore_sink_levels(const NodeInfo& sink, const RouteInfo& previous);

    // The sink node's currently negotiated format, e.g. "iec958 AC3 48000 Hz".
    std::string current_format(uint32_t node_id);

    bool failed() const;
    std::string error() const;

    struct Impl;

private:
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    std::unique_ptr<Impl> impl_;
};

// Rank the sinks that can carry a bitstream for the given codec, best first.
struct RankedSink {
    NodeInfo node;
    int score = 0;
    std::string reason;
};
std::vector<RankedSink> ranked_bitstream_sinks(const Snapshot& snap, Codec codec, bool allow_pcm_carrier);
// A short capability label for a sink, as the menus show it.
std::string capability_label(const Snapshot& snap, const NodeInfo& sink, Codec codec);
std::string port_type_of(const Snapshot& snap, const NodeInfo& sink);
bool is_digital_port(const std::string& port_type);

}  // namespace ac3spdif
