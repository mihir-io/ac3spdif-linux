// What the engine needs from a destination for the bitstream.
#pragma once

#include <cstdint>
#include <string>

namespace ac3spdif {

class BitstreamSink {
public:
    virtual ~BitstreamSink() = default;
    // Claim the device and negotiate the format. Split from begin() because
    // negotiation can take a noticeable fraction of a second, during which the
    // encoder keeps producing; measuring the buffer before this has settled
    // bakes the start-up transient into the steady-state latency.
    virtual void prepare() = 0;
    // Start pulling from the ring.
    virtual void begin() = 0;
    // Undo everything. Must tolerate a partly completed prepare().
    virtual void stop() = 0;

    virtual uint64_t rendered_bytes() const = 0;
    virtual uint64_t underrun_bytes() const = 0;
    virtual double bytes_per_second() const = 0;
    virtual std::string device_name() const = 0;
    virtual std::string carrier_label() const = 0;
    virtual std::string format_label() const = 0;
    virtual std::string stream_label() const = 0;
    virtual bool is_pcm_carrier() const = 0;
    // Frames the device asks for per request, which sets the smallest ring
    // that can satisfy it.
    virtual int request_frames() const = 0;
    // Measured delay between the ring and the wire, in frames.
    virtual int64_t device_delay_frames() = 0;
    virtual bool failed() const = 0;
    virtual std::string error() const = 0;
};

}  // namespace ac3spdif
