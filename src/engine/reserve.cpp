#include "engine/reserve.h"

#include <gio/gio.h>

#include <chrono>
#include <functional>
#include <future>
#include <thread>

#include "engine/config.h"
#include "engine/log.h"

namespace ac3spdif {

namespace {

constexpr guint kDoNotQueue = 4, kReplaceExisting = 2;

const char kIntrospection[] =
    "<node>"
    "  <interface name='org.freedesktop.ReserveDevice1'>"
    "    <method name='RequestRelease'>"
    "      <arg type='i' name='priority' direction='in'/>"
    "      <arg type='b' name='result' direction='out'/>"
    "    </method>"
    "    <property name='Priority' type='i' access='read'/>"
    "    <property name='ApplicationName' type='s' access='read'/>"
    "    <property name='ApplicationDeviceName' type='s' access='read'/>"
    "  </interface>"
    "</node>";

}  // namespace

struct DeviceReservation::Impl {
    int card_index;
    std::string app_name;
    int priority;
    std::string bus_name, object_path;

    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    std::thread thread;
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* node_info = nullptr;
    guint registration = 0;
    bool owned = false;

    // Run a function on the loop thread and wait for it.
    void invoke(const std::function<void()>& fn) {
        std::promise<void> done;
        auto future = done.get_future();
        struct Payload { const std::function<void()>* fn; std::promise<void>* done; };
        Payload payload{&fn, &done};
        g_main_context_invoke(context, [](gpointer data) -> gboolean {
            auto* p = static_cast<Payload*>(data);
            try { (*p->fn)(); } catch (...) { p->done->set_exception(std::current_exception()); return G_SOURCE_REMOVE; }
            p->done->set_value();
            return G_SOURCE_REMOVE;
        }, &payload);
        future.get();
    }

    guint request_name(guint flags) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "RequestName", g_variant_new("(su)", bus_name.c_str(), flags), G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
        if (!reply) {
            std::string message = error ? error->message : "unknown";
            g_clear_error(&error);
            throw Failure("D-Bus RequestName failed: " + message);
        }
        guint code = 0;
        g_variant_get(reply, "(u)", &code);
        g_variant_unref(reply);
        return code;
    }

    std::string owner_property(const char* property) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            connection, bus_name.c_str(), object_path.c_str(), "org.freedesktop.DBus.Properties",
            "Get", g_variant_new("(ss)", "org.freedesktop.ReserveDevice1", property),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
        if (!reply) { g_clear_error(&error); return ""; }
        GVariant* inner = nullptr;
        g_variant_get(reply, "(v)", &inner);
        std::string value;
        if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) value = g_variant_get_string(inner, nullptr);
        else if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_INT32)) value = std::to_string(g_variant_get_int32(inner));
        if (inner) g_variant_unref(inner);
        g_variant_unref(reply);
        return value;
    }

    bool request_release() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            connection, bus_name.c_str(), object_path.c_str(), "org.freedesktop.ReserveDevice1",
            "RequestRelease", g_variant_new("(i)", priority), G_VARIANT_TYPE("(b)"),
            G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
        if (!reply) { g_clear_error(&error); return false; }
        gboolean ok = FALSE;
        g_variant_get(reply, "(b)", &ok);
        g_variant_unref(reply);
        return ok;
    }

    static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar* method, GVariant*, GDBusMethodInvocation* invocation,
                            gpointer) {
        // Nothing outranks an interactive session holding a live bitstream.
        if (g_strcmp0(method, "RequestRelease") == 0)
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        else
            g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
    }

    static GVariant* get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                  const gchar* property, GError**, gpointer user_data) {
        auto* impl = static_cast<Impl*>(user_data);
        if (g_strcmp0(property, "Priority") == 0) return g_variant_new_int32(impl->priority);
        if (g_strcmp0(property, "ApplicationName") == 0) return g_variant_new_string(impl->app_name.c_str());
        if (g_strcmp0(property, "ApplicationDeviceName") == 0)
            return g_variant_new_string(fmt("hw:{}", impl->card_index).c_str());
        return nullptr;
    }
};

DeviceReservation::DeviceReservation(int card_index, std::string application_name, int priority)
    : impl_(std::make_unique<Impl>()) {
    impl_->card_index = card_index;
    impl_->app_name = std::move(application_name);
    impl_->priority = priority;
    impl_->bus_name = fmt("org.freedesktop.ReserveDevice1.Audio{}", card_index);
    impl_->object_path = fmt("/org/freedesktop/ReserveDevice1/Audio{}", card_index);
    impl_->context = g_main_context_new();
    impl_->loop = g_main_loop_new(impl_->context, FALSE);
    impl_->thread = std::thread([impl = impl_.get()] {
        g_main_context_push_thread_default(impl->context);
        g_main_loop_run(impl->loop);
        g_main_context_pop_thread_default(impl->context);
    });
}

DeviceReservation::~DeviceReservation() {
    try { release(); } catch (...) {}
    g_main_loop_quit(impl_->loop);
    if (impl_->thread.joinable()) impl_->thread.join();
    g_main_loop_unref(impl_->loop);
    g_main_context_unref(impl_->context);
}

bool DeviceReservation::held() const { return impl_->owned; }

std::string DeviceReservation::acquire() {
    std::string note;
    impl_->invoke([&] {
        Impl& i = *impl_;
        GError* error = nullptr;
        i.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!i.connection) {
            std::string message = error ? error->message : "unknown";
            g_clear_error(&error);
            throw Failure("no session D-Bus, so the card cannot be reserved: " + message);
        }
        guint code = i.request_name(kDoNotQueue);
        if (code == 1) {
            note = "the card was free";
        } else {
            std::string owner = i.owner_property("ApplicationName");
            std::string owner_priority = i.owner_property("Priority");
            if (owner.empty()) owner = "another program";
            if (!i.request_release())
                throw Failure(fmt("{} holds ALSA card {} (priority {}) and refused to release it",
                                  owner, i.card_index, owner_priority));
            // The old owner drops the name asynchronously; give it a moment.
            for (int attempt = 0; attempt < 40; ++attempt) {
                code = i.request_name(kDoNotQueue | kReplaceExisting);
                if (code == 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (code != 1) throw Failure(fmt("{} agreed to release card {} but never let go of it", owner, i.card_index));
            note = owner + " released it";
        }
        i.owned = true;
        i.node_info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
        static const GDBusInterfaceVTable vtable = {Impl::method_call, Impl::get_property, nullptr, {}};
        i.registration = g_dbus_connection_register_object(i.connection, i.object_path.c_str(),
                                                           i.node_info->interfaces[0], &vtable,
                                                           &i, nullptr, nullptr);
    });
    return note;
}

void DeviceReservation::release() {
    if (!impl_->connection) return;
    impl_->invoke([&] {
        Impl& i = *impl_;
        if (i.registration) {
            g_dbus_connection_unregister_object(i.connection, i.registration);
            i.registration = 0;
        }
        if (i.node_info) {
            g_dbus_node_info_unref(i.node_info);
            i.node_info = nullptr;
        }
        if (i.owned) {
            GVariant* reply = g_dbus_connection_call_sync(
                i.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                "ReleaseName", g_variant_new("(s)", i.bus_name.c_str()), G_VARIANT_TYPE("(u)"),
                G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
            if (reply) g_variant_unref(reply);
            i.owned = false;
        }
        g_object_unref(i.connection);
        i.connection = nullptr;
    });
}

}  // namespace ac3spdif
