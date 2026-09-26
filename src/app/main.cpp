// ac3spdif-app: the tray application. One instance per session; launching it
// again just shows the status window of the running one.

#include <glib-unix.h>
#include <gtk/gtk.h>

#include <csignal>

#include "app/app.h"
#include "engine/version.h"

int main(int argc, char** argv) {
    GtkApplication* application = gtk_application_new(AC3SPDIF_APP_ID, static_cast<GApplicationFlags>(0));
    ac3spdif::App* app = nullptr;
    g_signal_connect(application, "activate", G_CALLBACK(+[](GtkApplication* application, gpointer data) {
        auto** slot = static_cast<ac3spdif::App**>(data);
        if (!*slot) *slot = new ac3spdif::App(application);
        else (*slot)->show_status();
    }), &app);
    // Logout and the like send SIGTERM; stop the stream and restore the
    // devices instead of dying with the sink still switched over.
    for (int signal : {SIGTERM, SIGINT}) {
        g_unix_signal_add(signal, [](gpointer data) -> gboolean {
            auto** slot = static_cast<ac3spdif::App**>(data);
            if (*slot) (*slot)->quit(); else g_application_quit(g_application_get_default());
            return G_SOURCE_REMOVE;
        }, &app);
    }
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    delete app;
    g_object_unref(application);
    return status;
}
