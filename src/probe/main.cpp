// ac3spdif-probe: dump everything bitstream-relevant that PipeWire and ALSA
// know about, with the capable outputs called out. Reach for it whenever
// something will not engage, and paste it into a bug report.

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "engine/alsa_devices.h"
#include "engine/config.h"
#include "engine/pipewire_session.h"
#include "engine/version.h"

using namespace ac3spdif;

static std::string join(const std::vector<std::string>& items, const char* sep = " ") {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : sep) + i;
    return out;
}

int main() {
    std::printf("ac3spdif-probe %s\n\n", AC3SPDIF_VERSION);
    std::printf("libavcodec %s: ac3 encoder %s, dca encoder %s; libavformat spdif muxer %s\n\n",
                av_version_info(),
                avcodec_find_encoder_by_name("ac3") ? "yes" : "NO",
                avcodec_find_encoder_by_name("dca") ? "yes" : "NO",
                av_guess_format("spdif", nullptr, nullptr) ? "yes" : "NO");
    try {
        PwSession session;
        Snapshot snap = session.snapshot();
        std::printf("PipeWire %s (%s)\n", snap.server_version.c_str(), snap.server_name.c_str());
        std::printf("default sink: %s\ndefault source: %s\nconfigured default sink: %s\n\n",
                    snap.default_sink.c_str(), snap.default_source.c_str(), snap.configured_default_sink.c_str());
        for (const auto& n : snap.nodes) {
            std::printf("== node %u  %s\n", n.id, n.name.c_str());
            std::printf("   %s | %s | state %s | api %s%s\n", n.description.c_str(), n.media_class.c_str(),
                        n.state.c_str(), n.api().c_str(), n.is_virtual() ? " | virtual" : "");
            std::printf("   serial %llu | device.id %u | card.profile.device %d | channels %d | rate %d\n",
                        static_cast<unsigned long long>(n.serial), n.device_id(), n.profile_device(), n.channels, n.rate);
            std::printf("   position %s\n", n.prop("audio.position").c_str());
            std::printf("   iec958 codecs enabled: %s | offered: %s\n",
                        n.iec958_codecs.empty() ? "-" : join(n.iec958_codecs).c_str(),
                        join(std::vector<std::string>(n.offered_iec958_codecs.begin(), n.offered_iec958_codecs.end())).c_str());
            for (const auto& line : n.format_lines) std::printf("   format: %s\n", line.c_str());
            if (const RouteInfo* r = snap.route_for(n))
                std::printf("   route: %s (%s) port %s available %s codecs %s\n", r->name.c_str(), r->description.c_str(),
                            r->port_type.c_str(), r->available == 2 ? "yes" : r->available == 1 ? "no" : "unknown",
                            join(r->iec958_codecs).c_str());
        }
        std::printf("\n");
        for (const auto& d : snap.devices) {
            std::printf("== device %u  %s (%s) api %s\n", d.id, d.name.c_str(), d.description.c_str(), d.api.c_str());
            for (const auto& r : d.routes)
                std::printf("   active route %d dev %d %s %s port=%s available=%d codecs=[%s] volumes=%zu mute=%d\n",
                            r.index, r.device, r.direction.c_str(), r.name.c_str(), r.port_type.c_str(), r.available,
                            join(r.iec958_codecs).c_str(), r.channel_volumes.size(), r.mute ? 1 : 0);
            for (const auto& r : d.enum_routes)
                std::printf("   route %d %s %s (%s) port=%s available=%d\n", r.index, r.direction.c_str(), r.name.c_str(),
                            r.description.c_str(), r.port_type.c_str(), r.available);
        }
        std::printf("\n");
        for (Codec codec : {Codec::AC3, Codec::DTS}) {
            auto ranked = ranked_bitstream_sinks(snap, codec, true);
            std::printf("%s candidates, best first:\n", codec_short_name(codec));
            for (const auto& r : ranked)
                std::printf("   %3d  %s  (%s)\n", r.score, r.node.display_name().c_str(), r.reason.c_str());
            if (ranked.empty()) std::printf("   none\n");
        }
    } catch (const Failure& f) {
        std::printf("PipeWire: %s\n", f.what());
    }
    std::printf("\nALSA digital playback PCMs:\n");
    for (const auto& pcm : alsa_digital_pcms()) {
        std::printf("   %s  card %d\n      %s\n", pcm.name.c_str(), pcm.card_index, pcm.description.c_str());
        std::string status = alsa_iec958_status(pcm.card_index);
        if (!status.empty()) std::printf("      IEC958 status: %s\n", status.c_str());
    }
    return 0;
}
