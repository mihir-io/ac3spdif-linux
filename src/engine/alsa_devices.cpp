#include "engine/alsa_devices.h"

#include <alsa/asoundlib.h>

#include <cstdlib>
#include <cstring>

#include "engine/log.h"

namespace ac3spdif {

std::vector<AlsaPcm> alsa_digital_pcms() {
    std::vector<AlsaPcm> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return out;
    for (void** h = hints; *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");
        if (name && (std::strncmp(name, "iec958:", 7) == 0 || std::strncmp(name, "hdmi:", 5) == 0)
            && (!ioid || std::strcmp(ioid, "Output") == 0)) {
            AlsaPcm pcm;
            pcm.name = name;
            std::string d = desc ? desc : "";
            for (auto& c : d) if (c == '\n') c = ' ';
            pcm.description = d;
            pcm.card_index = alsa_card_index(pcm.name);
            out.push_back(pcm);
        }
        free(name);
        free(desc);
        free(ioid);
    }
    snd_device_name_free_hint(hints);
    return out;
}

int alsa_card_index(const std::string& pcm_name) {
    size_t at = pcm_name.find("CARD=");
    if (at != std::string::npos) {
        size_t end = pcm_name.find_first_of(",}", at);
        std::string card = pcm_name.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
        if (!card.empty() && std::isdigit(static_cast<unsigned char>(card[0]))) return std::atoi(card.c_str());
        return snd_card_get_index(card.c_str());
    }
    // hw:1,1 / plughw:1,1
    size_t colon = pcm_name.find(':');
    if (colon != std::string::npos && colon + 1 < pcm_name.size()
        && std::isdigit(static_cast<unsigned char>(pcm_name[colon + 1])))
        return std::atoi(pcm_name.c_str() + colon + 1);
    return -1;
}

std::string alsa_iec958_name(const std::string& pcm_name, int sample_rate) {
    if (pcm_name.find("AES0") != std::string::npos) return pcm_name;
    if (pcm_name.rfind("iec958:", 0) != 0 && pcm_name.rfind("hdmi:", 0) != 0) return pcm_name;
    // AES0: consumer, non-audio, copyright not asserted. AES1: original,
    // category "PCM coder". AES3: the sampling frequency code.
    const char* fs = sample_rate == 44100 ? "0x00" : sample_rate == 32000 ? "0x03" : "0x02";
    std::string sep = pcm_name.back() == ':' ? "" : ",";
    return pcm_name + sep + "AES0=0x06,AES1=0x82,AES2=0x00,AES3=" + fs;
}

std::string alsa_iec958_status(int card_index) {
    if (card_index < 0) return "";
    snd_ctl_t* ctl = nullptr;
    std::string hw = fmt("hw:{}", card_index);
    if (snd_ctl_open(&ctl, hw.c_str(), 0) < 0) return "";
    std::string out;
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&value);
    for (snd_ctl_elem_iface_t iface : {SND_CTL_ELEM_IFACE_MIXER, SND_CTL_ELEM_IFACE_PCM}) {
        snd_ctl_elem_id_clear(id);
        snd_ctl_elem_id_set_interface(id, iface);
        snd_ctl_elem_id_set_name(id, "IEC958 Playback Default");
        snd_ctl_elem_value_set_id(value, id);
        if (snd_ctl_elem_read(ctl, value) < 0) continue;
        snd_aes_iec958_t iec{};
        snd_ctl_elem_value_get_iec958(value, &iec);
        bool non_audio = iec.status[0] & 0x02;
        bool professional = iec.status[0] & 0x01;
        int fs = iec.status[3] & 0x0f;
        const char* rate = fs == 0x02 ? "48000" : fs == 0x00 ? "44100" : fs == 0x03 ? "32000" : "other";
        out = fmt("AES0=0x{:02x} AES1=0x{:02x} AES2=0x{:02x} AES3=0x{:02x} ({}, {}, {} Hz)",
                  iec.status[0], iec.status[1], iec.status[2], iec.status[3],
                  professional ? "professional" : "consumer", non_audio ? "non-audio" : "audio", rate);
        break;
    }
    snd_ctl_close(ctl);
    return out;
}

}  // namespace ac3spdif
