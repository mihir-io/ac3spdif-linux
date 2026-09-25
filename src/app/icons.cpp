#include "app/icons.h"

#include <glib.h>
#include <unistd.h>

#include <vector>

namespace ac3spdif {

std::string icon_directory() {
    std::vector<std::string> candidates;
    if (const char* env = g_getenv("AC3SPDIF_ICON_DIR")) candidates.push_back(env);
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        gchar* dir = g_path_get_dirname(self);
        candidates.push_back(std::string(dir) + "/../data/icons");
        candidates.push_back(std::string(dir) + "/../share/ac3spdif/icons");
        g_free(dir);
    }
#ifdef AC3SPDIF_SOURCE_DATADIR
    candidates.push_back(std::string(AC3SPDIF_SOURCE_DATADIR) + "/icons");
#endif
    candidates.push_back(std::string(g_get_user_data_dir()) + "/ac3spdif/icons");
#ifdef AC3SPDIF_DATADIR
    candidates.push_back(std::string(AC3SPDIF_DATADIR) + "/icons");
#endif
    candidates.push_back("/usr/share/ac3spdif/icons");
    for (const auto& dir : candidates) {
        std::string probe = dir + "/ac3spdif-idle-symbolic.svg";
        if (g_file_test(probe.c_str(), G_FILE_TEST_EXISTS)) {
            gchar* canonical = g_canonicalize_filename(dir.c_str(), nullptr);
            std::string out = canonical;
            g_free(canonical);
            return out;
        }
    }
    return "";
}

}  // namespace ac3spdif
