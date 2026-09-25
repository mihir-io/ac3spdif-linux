// The org.freedesktop.ReserveDevice1 protocol: how a program asks the sound
// server for an ALSA card, and holds it.
//
// WirePlumber (and PulseAudio before it) owns a well-known D-Bus name per card,
// Audio0, Audio1 and so on. Anyone who wants the hardware directly asks the
// current owner to release it; an owner with a lower priority hands the name
// over and closes the device, and takes it back when the name is released.
// JACK has used this to borrow cards from the desktop for fifteen years.
//
// The D-Bus work happens on a private GLib main context in its own thread, so
// the reservation neither needs nor disturbs any main loop the front end runs.
#pragma once

#include <memory>
#include <string>

namespace ac3spdif {

class DeviceReservation {
public:
    DeviceReservation(int card_index, std::string application_name, int priority = 0);
    ~DeviceReservation();

    // Take the card. Returns a note about what happened; throws Failure when
    // the current owner refuses.
    std::string acquire();
    void release();
    bool held() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3spdif
