// ALSA device discovery: the digital PCMs the direct transport can open.
#pragma once

#include <string>
#include <vector>

namespace ac3spdif {

struct AlsaPcm {
    std::string name;          // e.g. iec958:CARD=PCH,DEV=0
    std::string description;
    int card_index = -1;
};

// Every iec958: and hdmi: playback PCM ALSA knows about.
std::vector<AlsaPcm> alsa_digital_pcms();
// The card index behind a PCM name (CARD=name, CARD=N, hw:N,...), or -1.
int alsa_card_index(const std::string& pcm_name);
// The name with the IEC 958 channel-status arguments appended, unless it
// already carries them: consumer, non-audio, no copyright, and the rate.
std::string alsa_iec958_name(const std::string& pcm_name, int sample_rate);
// The four AES status bytes currently set on a card's digital output, as a
// human-readable line, or empty.
std::string alsa_iec958_status(int card_index);

}  // namespace ac3spdif
