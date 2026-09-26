#include "engine/pipewire_session.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/iec958-types.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include "engine/log.h"

namespace ac3spdif {

// --- small helpers -----------------------------------------------------------

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::map<std::string, std::string> dict_to_map(const spa_dict* dict) {
    std::map<std::string, std::string> out;
    if (!dict) return out;
    const spa_dict_item* item;
    spa_dict_for_each(item, dict) {
        if (item->key && item->value) out[item->key] = item->value;
    }
    return out;
}

std::string codec_name_of(uint32_t id) {
    const char* name = spa_debug_type_find_short_name(spa_type_audio_iec958_codec, id);
    return name ? name : "codec-" + std::to_string(id);
}

uint32_t codec_id_of(const std::string& name) {
    for (const spa_type_info* t = spa_type_audio_iec958_codec; t->name; ++t) {
        const char* slash = std::strrchr(t->name, ':');
        if (slash && name == slash + 1) return t->type;
    }
    return SPA_AUDIO_IEC958_CODEC_UNKNOWN;
}

// Values of a possibly-choice pod: (pointer to first value, count, choice type).
struct PodValues {
    const spa_pod* child = nullptr;
    uint32_t count = 0;
    uint32_t choice = SPA_CHOICE_None;
};

PodValues pod_values(const spa_pod* pod) {
    PodValues v;
    v.child = spa_pod_get_values(pod, &v.count, &v.choice);
    return v;
}

std::vector<uint32_t> id_values(const spa_pod* pod) {
    std::vector<uint32_t> out;
    PodValues v = pod_values(pod);
    if (!v.child || v.child->type != SPA_TYPE_Id) return out;
    const uint32_t* ids = static_cast<const uint32_t*>(SPA_POD_BODY(v.child));
    // For an enumeration the first value is the default, repeated in the list.
    for (uint32_t i = (v.choice == SPA_CHOICE_Enum && v.count > 1) ? 1 : 0; i < v.count; ++i)
        out.push_back(ids[i]);
    return out;
}

std::string rate_description(const spa_pod* pod) {
    PodValues v = pod_values(pod);
    if (!v.child || v.child->type != SPA_TYPE_Int) return "";
    const int32_t* vals = static_cast<const int32_t*>(SPA_POD_BODY(v.child));
    if (v.choice == SPA_CHOICE_Range && v.count >= 3) return fmt("{}-{} Hz", vals[1], vals[2]);
    if (v.choice == SPA_CHOICE_Enum && v.count > 1) {
        std::string out;
        for (uint32_t i = 1; i < v.count; ++i) out += (i > 1 ? "/" : "") + std::to_string(vals[i]);
        return out + " Hz";
    }
    return fmt("{} Hz", vals[0]);
}

int first_int(const spa_pod* pod) {
    PodValues v = pod_values(pod);
    if (!v.child || v.child->type != SPA_TYPE_Int || v.count == 0) return 0;
    return static_cast<const int32_t*>(SPA_POD_BODY(v.child))[0];
}

std::string json_string_field(const std::string& json, const std::string& field) {
    // Values here are tiny WirePlumber objects like {"name":"alsa_output..."}.
    std::string needle = "\"" + field + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    size_t colon = json.find(':', at + needle.size());
    if (colon == std::string::npos) return "";
    size_t open = json.find('"', colon);
    if (open == std::string::npos) return "";
    size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return "";
    return json.substr(open + 1, close - open - 1);
}

void parse_props_object(const spa_pod* props, std::vector<std::string>* codecs,
                        std::vector<float>* volumes, bool* mute) {
    if (!props || !spa_pod_is_object(props)) return;
    if (codecs) {
        codecs->clear();
        if (const spa_pod_prop* p = spa_pod_find_prop(props, nullptr, SPA_PROP_iec958Codecs)) {
            uint32_t n = 0;
            const uint32_t* ids = static_cast<const uint32_t*>(spa_pod_get_array(&p->value, &n));
            for (uint32_t i = 0; ids && i < n; ++i) codecs->push_back(codec_name_of(ids[i]));
        }
    }
    if (volumes) {
        volumes->clear();
        if (const spa_pod_prop* p = spa_pod_find_prop(props, nullptr, SPA_PROP_channelVolumes)) {
            uint32_t n = 0;
            const float* vals = static_cast<const float*>(spa_pod_get_array(&p->value, &n));
            for (uint32_t i = 0; vals && i < n; ++i) volumes->push_back(vals[i]);
        }
    }
    if (mute) {
        if (const spa_pod_prop* p = spa_pod_find_prop(props, nullptr, SPA_PROP_mute)) {
            bool value = false;
            if (spa_pod_get_bool(&p->value, &value) == 0) *mute = value;
        }
    }
}

bool parse_route(const spa_pod* pod, RouteInfo& r) {
    int32_t index = -1, device = -1;
    uint32_t direction = 0, available = 0;
    const char* name = nullptr;
    const char* description = nullptr;
    const spa_pod* info = nullptr;
    const spa_pod* props = nullptr;
    const spa_pod* profiles = nullptr;
    if (spa_pod_parse_object(pod, SPA_TYPE_OBJECT_ParamRoute, nullptr,
                             SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
                             SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
                             SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&device),
                             SPA_PARAM_ROUTE_name, SPA_POD_String(&name),
                             SPA_PARAM_ROUTE_description, SPA_POD_OPT_String(&description),
                             SPA_PARAM_ROUTE_available, SPA_POD_OPT_Id(&available),
                             SPA_PARAM_ROUTE_info, SPA_POD_OPT_Pod(&info),
                             SPA_PARAM_ROUTE_profiles, SPA_POD_OPT_Pod(&profiles),
                             SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
        return false;
    r.index = index;
    r.device = device;
    r.direction = direction == SPA_DIRECTION_OUTPUT ? "Output" : "Input";
    r.name = name ? name : "";
    r.description = description ? description : "";
    r.available = static_cast<int>(available);
    if (info) {
        spa_pod_parser parser;
        spa_pod_frame frame;
        spa_pod_parser_pod(&parser, info);
        if (spa_pod_parser_push_struct(&parser, &frame) == 0) {
            int32_t n = 0;
            if (spa_pod_parser_get_int(&parser, &n) == 0) {
                for (int32_t i = 0; i < n; ++i) {
                    const char* key = nullptr;
                    const char* value = nullptr;
                    if (spa_pod_parser_get(&parser, SPA_POD_String(&key), SPA_POD_String(&value), 0) < 0)
                        break;
                    if (key && value) r.info[key] = value;
                }
            }
            spa_pod_parser_pop(&parser, &frame);
        }
    }
    auto it = r.info.find("port.type");
    r.port_type = it == r.info.end() ? "" : lower(it->second);
    if (profiles) {
        uint32_t n = 0;
        const int32_t* vals = static_cast<const int32_t*>(spa_pod_get_array(profiles, &n));
        for (uint32_t i = 0; vals && i < n; ++i) r.profiles.push_back(vals[i]);
    }
    if (props) {
        r.has_props = true;
        parse_props_object(props, &r.iec958_codecs, &r.channel_volumes, &r.mute);
    }
    return true;
}

std::string format_line(const spa_pod* param, NodeInfo& node) {
    uint32_t media_type = 0, media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return "";
    if (media_type != SPA_MEDIA_TYPE_audio) return "";
    if (media_subtype == SPA_MEDIA_SUBTYPE_iec958) {
        std::string codecs, rates;
        if (const spa_pod_prop* p = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_iec958Codec)) {
            std::set<std::string> seen;
            for (uint32_t id : id_values(&p->value)) {
                std::string name = codec_name_of(id);
                if (seen.insert(name).second) codecs += (codecs.empty() ? "" : " ") + name;
                node.offered_iec958_codecs.insert(name);
            }
        }
        if (const spa_pod_prop* p = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate))
            rates = rate_description(&p->value);
        return "iec958: " + codecs + (rates.empty() ? "" : ", " + rates);
    }
    if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        std::string format, rates;
        int channels = 0;
        if (const spa_pod_prop* p = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_format)) {
            auto ids = id_values(&p->value);
            PodValues v = pod_values(&p->value);
            uint32_t id = ids.empty() ? 0 : ids[0];
            if (v.choice == SPA_CHOICE_Enum && v.child && v.count > 0)
                id = static_cast<const uint32_t*>(SPA_POD_BODY(v.child))[0];
            const char* name = spa_debug_type_find_short_name(spa_type_audio_format, id);
            format = name ? name : "?";
        }
        if (const spa_pod_prop* p = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_channels))
            channels = first_int(&p->value);
        if (const spa_pod_prop* p = spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate)) {
            rates = rate_description(&p->value);
            if (node.rate == 0) node.rate = first_int(&p->value);
        }
        if (channels > 0 && node.channels == 0) node.channels = channels;
        return fmt("pcm: {} {}ch {}", format, channels, rates);
    }
    return "";
}

}  // namespace

// --- model helpers -----------------------------------------------------------

std::string NodeInfo::base_name() const {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return name;
    for (size_t i = dot + 1; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    return name.substr(0, dot);
}

uint32_t NodeInfo::device_id() const {
    std::string v = prop("device.id");
    return v.empty() ? 0 : static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
}

int NodeInfo::profile_device() const {
    std::string v = prop("card.profile.device");
    return v.empty() ? -1 : std::atoi(v.c_str());
}

bool NodeInfo::has_codec(Codec c) const {
    return std::find(iec958_codecs.begin(), iec958_codecs.end(), codec_pipewire_name(c))
        != iec958_codecs.end();
}

const NodeInfo* Snapshot::node_by_id(uint32_t id) const {
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

const DeviceInfo* Snapshot::device_by_id(uint32_t id) const {
    for (const auto& d : devices) if (d.id == id) return &d;
    return nullptr;
}

const RouteInfo* Snapshot::route_for(const NodeInfo& sink) const {
    const DeviceInfo* device = device_by_id(sink.device_id());
    if (!device) return nullptr;
    int profile_device = sink.profile_device();
    for (const auto& r : device->routes)
        if (r.direction == "Output" && (profile_device < 0 || r.device == profile_device)) return &r;
    return nullptr;
}

std::vector<const NodeInfo*> Snapshot::sinks() const {
    std::vector<const NodeInfo*> out;
    for (const auto& n : nodes) if (n.is_sink()) out.push_back(&n);
    return out;
}

std::vector<const NodeInfo*> Snapshot::sources() const {
    std::vector<const NodeInfo*> out;
    for (const auto& n : nodes) if (n.is_source()) out.push_back(&n);
    return out;
}

// --- the implementation ------------------------------------------------------

struct NodeEntry {
    PwSession::Impl* owner = nullptr;
    pw_proxy* proxy = nullptr;
    spa_hook listener{};
    NodeInfo info;
    std::string current_format;
};

struct DeviceEntry {
    PwSession::Impl* owner = nullptr;
    pw_proxy* proxy = nullptr;
    spa_hook listener{};
    DeviceInfo info;
};

struct PwSession::Impl {
    PwSession* session = nullptr;
    pw_registry* registry = nullptr;
    spa_hook registry_listener{};
    spa_hook core_listener{};
    std::map<uint32_t, std::unique_ptr<NodeEntry>> nodes;
    std::map<uint32_t, std::unique_ptr<DeviceEntry>> devices;
    pw_proxy* metadata = nullptr;
    uint32_t metadata_id = 0;
    spa_hook metadata_listener{};
    std::map<std::string, std::string> metadata_values;
    pw_proxy* virtual_sink = nullptr;
    int done_seq = -1;
    std::string server_version;
    std::string server_name;
    std::string last_error;
    std::string error;   // fatal: the connection is gone

    void destroy_proxies();
};

namespace {

void node_param(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
    auto* entry = static_cast<NodeEntry*>(data);
    if (!param) return;
    if (id == SPA_PARAM_Props) {
        parse_props_object(param, &entry->info.iec958_codecs, nullptr, nullptr);
    } else if (id == SPA_PARAM_EnumFormat) {
        std::string line = format_line(param, entry->info);
        if (!line.empty()) entry->info.format_lines.push_back(line);
    } else if (id == SPA_PARAM_Format) {
        NodeInfo scratch;
        entry->current_format = format_line(param, scratch);
    }
}

void node_info(void* data, const pw_node_info* info) {
    auto* entry = static_cast<NodeEntry*>(data);
    NodeInfo& n = entry->info;
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
        n.props = dict_to_map(info->props);
        n.name = n.prop(PW_KEY_NODE_NAME);
        n.description = n.prop(PW_KEY_NODE_DESCRIPTION);
        n.nick = n.prop(PW_KEY_NODE_NICK);
        n.media_class = n.prop(PW_KEY_MEDIA_CLASS);
        std::string serial = n.prop(PW_KEY_OBJECT_SERIAL);
        if (!serial.empty()) n.serial = std::strtoull(serial.c_str(), nullptr, 10);
        std::string channels = n.prop("audio.channels");
        if (!channels.empty()) n.channels = std::atoi(channels.c_str());
    }
    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE)
        n.state = pw_node_state_as_string(info->state);
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
        for (uint32_t i = 0; i < info->n_params; ++i) {
            const spa_param_info& p = info->params[i];
            if (!(p.flags & SPA_PARAM_INFO_READ)) continue;
            if (p.id == SPA_PARAM_Props) {
                n.iec958_codecs.clear();
                pw_node_enum_params(reinterpret_cast<pw_node*>(entry->proxy), 0, SPA_PARAM_Props,
                                    0, UINT32_MAX, nullptr);
            } else if (p.id == SPA_PARAM_EnumFormat) {
                n.format_lines.clear();
                n.offered_iec958_codecs.clear();
                pw_node_enum_params(reinterpret_cast<pw_node*>(entry->proxy), 0,
                                    SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
            }
        }
    }
}

const pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info,
    .param = node_param,
};

void device_param(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
    auto* entry = static_cast<DeviceEntry*>(data);
    if (!param) return;
    RouteInfo route;
    if (!parse_route(param, route)) return;
    if (id == SPA_PARAM_Route) entry->info.routes.push_back(route);
    else if (id == SPA_PARAM_EnumRoute) entry->info.enum_routes.push_back(route);
}

void device_info(void* data, const pw_device_info* info) {
    auto* entry = static_cast<DeviceEntry*>(data);
    DeviceInfo& d = entry->info;
    if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) {
        d.props = dict_to_map(info->props);
        d.name = d.prop(PW_KEY_DEVICE_NAME);
        d.description = d.prop(PW_KEY_DEVICE_DESCRIPTION);
        d.api = d.prop(PW_KEY_DEVICE_API);
    }
    if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
        for (uint32_t i = 0; i < info->n_params; ++i) {
            const spa_param_info& p = info->params[i];
            if (!(p.flags & SPA_PARAM_INFO_READ)) continue;
            if (p.id == SPA_PARAM_Route) {
                d.routes.clear();
                pw_device_enum_params(reinterpret_cast<pw_device*>(entry->proxy), 0, SPA_PARAM_Route,
                                      0, UINT32_MAX, nullptr);
            } else if (p.id == SPA_PARAM_EnumRoute) {
                d.enum_routes.clear();
                pw_device_enum_params(reinterpret_cast<pw_device*>(entry->proxy), 0,
                                      SPA_PARAM_EnumRoute, 0, UINT32_MAX, nullptr);
            }
        }
    }
}

const pw_device_events device_events = {
    .version = PW_VERSION_DEVICE_EVENTS,
    .info = device_info,
    .param = device_param,
};

int metadata_property(void* data, uint32_t subject, const char* key, const char*, const char* value) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    if (subject != PW_ID_CORE || !key) return 0;
    if (value) impl->metadata_values[key] = value;
    else impl->metadata_values.erase(key);
    return 0;
}

const pw_metadata_events metadata_events = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

void registry_global(void* data, uint32_t id, uint32_t, const char* type, uint32_t,
                     const spa_dict* props) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class || !spa_strstartswith(media_class, "Audio/")) return;
        auto entry = std::make_unique<NodeEntry>();
        entry->owner = impl;
        entry->info.id = id;
        entry->info.props = dict_to_map(props);
        entry->info.name = entry->info.prop(PW_KEY_NODE_NAME);
        entry->info.media_class = media_class;
        entry->proxy = static_cast<pw_proxy*>(
            pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, 0));
        if (!entry->proxy) return;
        pw_node_add_listener(reinterpret_cast<pw_node*>(entry->proxy), &entry->listener,
                             &node_events, entry.get());
        impl->nodes[id] = std::move(entry);
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Device)) {
        const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class || !spa_streq(media_class, "Audio/Device")) return;
        auto entry = std::make_unique<DeviceEntry>();
        entry->owner = impl;
        entry->info.id = id;
        entry->proxy = static_cast<pw_proxy*>(
            pw_registry_bind(impl->registry, id, type, PW_VERSION_DEVICE, 0));
        if (!entry->proxy) return;
        pw_device_add_listener(reinterpret_cast<pw_device*>(entry->proxy), &entry->listener,
                               &device_events, entry.get());
        impl->devices[id] = std::move(entry);
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || !spa_streq(name, "default") || impl->metadata) return;
        impl->metadata = static_cast<pw_proxy*>(
            pw_registry_bind(impl->registry, id, type, PW_VERSION_METADATA, 0));
        if (!impl->metadata) return;
        impl->metadata_id = id;
        pw_metadata_add_listener(reinterpret_cast<pw_metadata*>(impl->metadata),
                                 &impl->metadata_listener, &metadata_events, impl);
    }
}

void registry_global_remove(void* data, uint32_t id) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    if (auto it = impl->nodes.find(id); it != impl->nodes.end()) {
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        impl->nodes.erase(it);
    } else if (auto dt = impl->devices.find(id); dt != impl->devices.end()) {
        spa_hook_remove(&dt->second->listener);
        pw_proxy_destroy(dt->second->proxy);
        impl->devices.erase(dt);
    } else if (impl->metadata && id == impl->metadata_id) {
        spa_hook_remove(&impl->metadata_listener);
        pw_proxy_destroy(impl->metadata);
        impl->metadata = nullptr;
        impl->metadata_id = 0;
    }
}

const pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void core_info(void* data, const pw_core_info* info) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    if (info->version) impl->server_version = info->version;
    if (info->name) impl->server_name = info->name;
}

void core_done(void* data, uint32_t id, int seq) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    if (id == PW_ID_CORE) {
        impl->done_seq = seq;
        pw_thread_loop_signal(impl->session->loop(), false);
    }
}

void core_error(void* data, uint32_t id, int seq, int res, const char* message) {
    auto* impl = static_cast<PwSession::Impl*>(data);
    std::string text = fmt("PipeWire error on object {} (seq {}): {} ({})", id, seq,
                           message ? message : "", spa_strerror(res));
    impl->last_error = text;
    if (id == PW_ID_CORE && res == -EPIPE) impl->error = "disconnected from PipeWire";
    pw_thread_loop_signal(impl->session->loop(), false);
}

const pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = core_info,
    .done = core_done,
    .error = core_error,
};

}  // namespace

void PwSession::Impl::destroy_proxies() {
    for (auto& [id, entry] : nodes) {
        spa_hook_remove(&entry->listener);
        pw_proxy_destroy(entry->proxy);
    }
    nodes.clear();
    for (auto& [id, entry] : devices) {
        spa_hook_remove(&entry->listener);
        pw_proxy_destroy(entry->proxy);
    }
    devices.clear();
    if (metadata) {
        spa_hook_remove(&metadata_listener);
        pw_proxy_destroy(metadata);
        metadata = nullptr;
    }
    if (virtual_sink) {
        pw_proxy_destroy(virtual_sink);
        virtual_sink = nullptr;
    }
    if (registry) {
        spa_hook_remove(&registry_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        registry = nullptr;
    }
}

// --- PwSession ---------------------------------------------------------------

namespace {

// media.category = Manager is what a mixer or patchbay declares, and it is
// what this client is: it creates a sink and makes it the default. It also
// matters inside a Flatpak, where WirePlumber fences a sandboxed client off
// from writing metadata (so from changing the default sink) unless the client
// declares itself a manager. Outside a sandbox every local client already
// has full access.
pw_properties* client_properties() {
    return pw_properties_new(PW_KEY_APP_NAME, "ac3spdif",
                             PW_KEY_APP_ID, "org.ac3spdif",
                             PW_KEY_APP_ICON_NAME, "ac3spdif",
                             PW_KEY_MEDIA_CATEGORY, "Manager", nullptr);
}

}  // namespace

PwSession::Lock::Lock(PwSession& s) : session(s) { pw_thread_loop_lock(s.loop_); }
PwSession::Lock::~Lock() { pw_thread_loop_unlock(session.loop_); }

PwSession::PwSession() : impl_(std::make_unique<Impl>()) {
    impl_->session = this;
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("ac3spdif", nullptr);
    if (!loop_) throw Failure("cannot create the PipeWire thread loop");
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) throw Failure("cannot create a PipeWire context");
    if (pw_thread_loop_start(loop_) < 0) throw Failure("cannot start the PipeWire thread loop");

    auto guard = lock();
    core_ = pw_context_connect(context_, client_properties(), 0);
    if (!core_)
        throw Failure(fmt("cannot connect to PipeWire: {} (is the pipewire user service running?)",
                          std::strerror(errno)));
    pw_core_add_listener(core_, &impl_->core_listener, &core_events, impl_.get());
    impl_->registry = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(impl_->registry, &impl_->registry_listener, &registry_events,
                             impl_.get());
    // Globals, then their info, then their params: three roundtrips settle it.
    sync();
    sync();
    sync();
}

PwSession::~PwSession() {
    if (loop_) {
        {
            auto guard = lock();
            impl_->destroy_proxies();
            spa_hook_remove(&impl_->core_listener);
            if (core_) pw_core_disconnect(core_);
            core_ = nullptr;
        }
        pw_thread_loop_stop(loop_);
    }
    if (context_) pw_context_destroy(context_);
    if (loop_) pw_thread_loop_destroy(loop_);
}

void PwSession::wait() { pw_thread_loop_wait(loop_); }
void PwSession::signal() { pw_thread_loop_signal(loop_, false); }

void PwSession::sync() {
    if (!core_) return;
    int pending = pw_core_sync(core_, PW_ID_CORE, 0);
    while (impl_->done_seq != pending) {
        if (!impl_->error.empty()) throw Failure(impl_->error);
        if (pw_thread_loop_timed_wait(loop_, 5) == -ETIMEDOUT)
            throw Failure("PipeWire did not answer within 5 seconds");
    }
}

Snapshot PwSession::snapshot() {
    auto guard = lock();
    sync();
    sync();
    sync();
    Snapshot snap;
    for (const auto& [id, entry] : impl_->nodes) snap.nodes.push_back(entry->info);
    for (const auto& [id, entry] : impl_->devices) snap.devices.push_back(entry->info);
    auto meta = [&](const char* key) {
        auto it = impl_->metadata_values.find(key);
        return it == impl_->metadata_values.end() ? std::string() : json_string_field(it->second, "name");
    };
    snap.default_sink = meta("default.audio.sink");
    snap.default_source = meta("default.audio.source");
    snap.configured_default_sink = meta("default.configured.audio.sink");
    snap.server_version = impl_->server_version;
    snap.server_name = impl_->server_name;
    return snap;
}

std::optional<NodeInfo> PwSession::node_by_id(uint32_t id) {
    auto guard = lock();
    sync();
    auto it = impl_->nodes.find(id);
    if (it == impl_->nodes.end()) return std::nullopt;
    return it->second->info;
}

std::optional<NodeInfo> PwSession::find_node(const std::string& query, NodeRole role) {
    Snapshot snap = snapshot();
    std::vector<const NodeInfo*> candidates;
    for (const auto& n : snap.nodes) {
        bool ok = role == NodeRole::Any || (role == NodeRole::Sink && n.is_sink())
                  || (role == NodeRole::Source && n.is_source());
        if (ok) candidates.push_back(&n);
    }
    if (query.empty()) return std::nullopt;
    // Exact name, then exact name with the ".N" recreation suffix ignored, then
    // id, description and nick, then case-insensitive prefix and substring.
    for (auto* n : candidates) if (n->name == query) return *n;
    for (auto* n : candidates) if (n->base_name() == query) return *n;
    if (std::all_of(query.begin(), query.end(), [](unsigned char c) { return std::isdigit(c); })) {
        uint32_t id = static_cast<uint32_t>(std::strtoul(query.c_str(), nullptr, 10));
        for (auto* n : candidates) if (n->id == id) return *n;
    }
    for (auto* n : candidates) if (n->description == query || n->nick == query) return *n;
    std::string lowered = lower(query);
    for (auto* n : candidates)
        if (lower(n->name).rfind(lowered, 0) == 0 || lower(n->description).rfind(lowered, 0) == 0)
            return *n;
    for (auto* n : candidates)
        if (lower(n->name).find(lowered) != std::string::npos
            || lower(n->description).find(lowered) != std::string::npos)
            return *n;
    return std::nullopt;
}

NodeInfo PwSession::create_virtual_sink(const std::string& name, const std::string& description,
                                        int channels, int sample_rate) {
    auto guard = lock();
    std::string positions = "[";
    for (const auto& p : channel_position_names(channels)) positions += " " + p;
    positions += " ]";
    pw_properties* props = pw_properties_new(
        PW_KEY_FACTORY_NAME, "support.null-audio-sink",
        PW_KEY_NODE_NAME, name.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_NICK, "AC3SPDIF",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_OBJECT_LINGER, "false",
        PW_KEY_DEVICE_ICON_NAME, "audio-card",
        "audio.position", positions.c_str(),
        "audio.channels", std::to_string(channels).c_str(),
        "audio.rate", std::to_string(sample_rate).c_str(),
        // The desktop volume control applies to what the monitor carries, so
        // the volume keys behave the way they do with a real output.
        "monitor.channel-volumes", "true",
        nullptr);
    impl_->virtual_sink = static_cast<pw_proxy*>(pw_core_create_object(
        core_, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
    pw_properties_free(props);
    if (!impl_->virtual_sink) throw Failure("PipeWire refused to create the virtual sink");
    for (int attempt = 0; attempt < 20; ++attempt) {
        sync();
        for (const auto& [id, entry] : impl_->nodes) {
            if (entry->info.name == name && entry->info.is_virtual()) {
                sync();
                sync();
                return entry->info;
            }
        }
        if (!impl_->last_error.empty()) {
            std::string message = impl_->last_error;
            impl_->last_error.clear();
            throw Failure("cannot create the virtual sink: " + message);
        }
    }
    throw Failure("PipeWire accepted the virtual sink but never announced it");
}

void PwSession::destroy_virtual_sink() {
    auto guard = lock();
    if (impl_->virtual_sink) {
        pw_proxy_destroy(impl_->virtual_sink);
        impl_->virtual_sink = nullptr;
        sync();
    }
}

std::string PwSession::configured_default_sink() {
    auto guard = lock();
    sync();
    auto it = impl_->metadata_values.find("default.configured.audio.sink");
    return it == impl_->metadata_values.end() ? std::string() : it->second;
}

void PwSession::set_configured_default_sink(const std::string& name) {
    auto guard = lock();
    if (!impl_->metadata) throw Failure("no default metadata object; is WirePlumber running?");
    std::string json = name.rfind('{', 0) == 0 ? name : "{ \"name\": \"" + name + "\" }";
    pw_metadata_set_property(reinterpret_cast<pw_metadata*>(impl_->metadata), PW_ID_CORE,
                             "default.configured.audio.sink", "Spa:String:JSON", json.c_str());
    sync();
}

void PwSession::clear_configured_default_sink() {
    auto guard = lock();
    if (!impl_->metadata) return;
    pw_metadata_set_property(reinterpret_cast<pw_metadata*>(impl_->metadata), PW_ID_CORE,
                             "default.configured.audio.sink", nullptr, nullptr);
    sync();
}

namespace {

// Build a Props object holding some of: iec958Codecs, channelVolumes, mute.
const spa_pod* build_props(spa_pod_builder* b, const std::vector<uint32_t>* codec_ids,
                           const std::vector<float>* volumes, const bool* mute) {
    spa_pod_frame frame;
    spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    if (codec_ids) {
        spa_pod_builder_prop(b, SPA_PROP_iec958Codecs, 0);
        spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Id,
                              static_cast<uint32_t>(codec_ids->size()), codec_ids->data());
    }
    if (volumes && !volumes->empty()) {
        spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float,
                              static_cast<uint32_t>(volumes->size()), volumes->data());
    }
    if (mute) {
        spa_pod_builder_prop(b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(b, *mute);
    }
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &frame));
}

}  // namespace

// Apply a Props object to a sink: through its device route when it has one,
// so WirePlumber sees and (when asked) persists it, otherwise on the node.
static bool apply_sink_props(PwSession::Impl* impl, const NodeInfo& sink, bool save,
                             const std::vector<uint32_t>* codec_ids,
                             const std::vector<float>* volumes, const bool* mute) {
    uint8_t buffer[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    auto device_it = impl->devices.find(sink.device_id());
    if (device_it != impl->devices.end()) {
        int profile_device = sink.profile_device();
        for (const auto& route : device_it->second->info.routes) {
            if (route.direction != "Output") continue;
            if (profile_device >= 0 && route.device != profile_device) continue;
            spa_pod_frame frame;
            spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
            spa_pod_builder_add(&b,
                                SPA_PARAM_ROUTE_index, SPA_POD_Int(route.index),
                                SPA_PARAM_ROUTE_device, SPA_POD_Int(route.device),
                                SPA_PARAM_ROUTE_save, SPA_POD_Bool(save), 0);
            spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
            build_props(&b, codec_ids, volumes, mute);
            const spa_pod* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));
            pw_device_set_param(reinterpret_cast<pw_device*>(device_it->second->proxy),
                                SPA_PARAM_Route, 0, pod);
            return true;
        }
    }
    auto node_it = impl->nodes.find(sink.id);
    if (node_it == impl->nodes.end()) return false;
    const spa_pod* pod = build_props(&b, codec_ids, volumes, mute);
    pw_node_set_param(reinterpret_cast<pw_node*>(node_it->second->proxy), SPA_PARAM_Props, 0, pod);
    return true;
}

bool PwSession::enable_iec958_codecs(const NodeInfo& sink, const std::vector<std::string>& codecs) {
    auto guard = lock();
    std::vector<uint32_t> ids;
    std::set<std::string> wanted(codecs.begin(), codecs.end());
    wanted.insert("PCM");
    for (const auto& existing : sink.iec958_codecs) wanted.insert(existing);
    for (const auto& name : wanted) {
        uint32_t id = codec_id_of(name);
        if (id != SPA_AUDIO_IEC958_CODEC_UNKNOWN) ids.push_back(id);
    }
    if (!apply_sink_props(impl_.get(), sink, true, &ids, nullptr, nullptr)) return false;
    // The node re-announces its params; give that time to land.
    for (int attempt = 0; attempt < 4; ++attempt) {
        sync();
        auto it = impl_->nodes.find(sink.id);
        if (it == impl_->nodes.end()) return false;
        bool all = true;
        for (const auto& name : codecs) {
            const auto& have = it->second->info.iec958_codecs;
            if (std::find(have.begin(), have.end(), name) == have.end()) all = false;
        }
        if (all) return true;
    }
    return false;
}

std::optional<RouteInfo> PwSession::neutralise_sink_levels(const NodeInfo& sink) {
    auto guard = lock();
    sync();
    RouteInfo previous;
    auto device_it = impl_->devices.find(sink.device_id());
    if (device_it != impl_->devices.end()) {
        for (const auto& route : device_it->second->info.routes)
            if (route.direction == "Output" && (sink.profile_device() < 0 || route.device == sink.profile_device()))
                previous = route;
    }
    int channels = std::max(sink.channels, 2);
    if (!previous.channel_volumes.empty()) channels = static_cast<int>(previous.channel_volumes.size());
    std::vector<float> unity(static_cast<size_t>(channels), 1.0f);
    bool unmuted = false;
    if (!apply_sink_props(impl_.get(), sink, false, nullptr, &unity, &unmuted)) return std::nullopt;
    sync();
    return previous.has_props ? std::optional<RouteInfo>(previous) : std::nullopt;
}

void PwSession::restore_sink_levels(const NodeInfo& sink, const RouteInfo& previous) {
    auto guard = lock();
    if (previous.channel_volumes.empty()) return;
    apply_sink_props(impl_.get(), sink, false, nullptr, &previous.channel_volumes, &previous.mute);
    sync();
}

std::string PwSession::current_format(uint32_t node_id) {
    auto guard = lock();
    auto it = impl_->nodes.find(node_id);
    if (it == impl_->nodes.end()) return "";
    it->second->current_format.clear();
    pw_node_enum_params(reinterpret_cast<pw_node*>(it->second->proxy), 0, SPA_PARAM_Format, 0, 1,
                        nullptr);
    sync();
    sync();
    it = impl_->nodes.find(node_id);
    return it == impl_->nodes.end() ? "" : it->second->current_format;
}

// --- ranking -----------------------------------------------------------------

bool is_digital_port(const std::string& port_type) {
    return port_type == "spdif" || port_type == "hdmi" || port_type == "hdmi-out"
        || port_type == "displayport" || port_type == "iec958";
}

std::string port_type_of(const Snapshot& snap, const NodeInfo& sink) {
    if (const RouteInfo* route = snap.route_for(sink)) return route->port_type;
    // A sink without routes: guess from the profile name PipeWire gave it.
    std::string name = lower(sink.name);
    if (name.find("iec958") != std::string::npos) return "spdif";
    if (name.find("hdmi") != std::string::npos) return "hdmi";
    return "";
}

std::vector<RankedSink> ranked_bitstream_sinks(const Snapshot& snap, Codec codec,
                                               bool allow_pcm_carrier) {
    std::vector<RankedSink> out;
    for (const NodeInfo* sink : snap.sinks()) {
        if (sink->media_class != "Audio/Sink") continue;
        if (sink->is_virtual()) continue;
        std::string api = sink->api();
        if (api == "bluez5" || api == "raop" || api == "jack") continue;
        std::string port = port_type_of(snap, *sink);
        const RouteInfo* route = snap.route_for(*sink);
        RankedSink ranked{*sink, 0, ""};
        if (sink->has_codec(codec)) {
            ranked.score = 100;
            ranked.reason = "advertises " + std::string(codec_pipewire_name(codec));
        } else if (api == "alsa" && is_digital_port(port)) {
            ranked.score = 80;
            ranked.reason = "digital output, codec can be enabled";
        } else if (allow_pcm_carrier && is_digital_port(port)) {
            ranked.score = 50;
            ranked.reason = "PCM carrier";
        } else {
            continue;
        }
        if (port == "spdif") ranked.score += 10;
        else if (port == "hdmi") ranked.score += 5;
        if (route && route->available == 1) ranked.score -= 60;   // nothing plugged in
        out.push_back(ranked);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RankedSink& a, const RankedSink& b) { return a.score > b.score; });
    return out;
}

std::string capability_label(const Snapshot& snap, const NodeInfo& sink, Codec codec) {
    std::string port = port_type_of(snap, sink);
    std::string api = sink.api();
    if (sink.has_codec(codec)) return "✓ passthrough";
    if (api == "alsa" && is_digital_port(port)) return "✓ passthrough (enable)";
    if (is_digital_port(port)) return "~ PCM carrier";
    if (api == "bluez5" || api == "raop") return "✗ " + api;
    if (sink.is_virtual()) return "✗ virtual";
    return "✗ analog";
}

}  // namespace ac3spdif

namespace ac3spdif {
bool PwSession::failed() const { return !impl_->error.empty(); }
std::string PwSession::error() const { return impl_->error; }
}  // namespace ac3spdif
