// The drift controller's arithmetic: proportional, capped, and scaled by the
// block length so the ppm ceiling is the same for both codecs.
#include "engine/encoder.h"
#include "engine/ring.h"

#include <cstdio>

using namespace ac3spdif;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    ByteRing pcm(1 << 20), spdif(1 << 20);
    Config ac3;
    Encoder a(ac3, pcm, spdif);
    CHECK(a.max_correction_frames() == 8);
    CHECK(a.correction_frames(100) == 1);              // less than a burst off: one sample
    CHECK(a.correction_frames(6144 * 3) == 3);         // three bursts off: three samples
    CHECK(a.correction_frames(6144 * 40) == 8);        // capped

    Config dts;
    dts.codec = Codec::DTS;
    Encoder d(dts, pcm, spdif);
    CHECK(d.max_correction_frames() == 2);             // 8 * 512 / 1536
    CHECK(d.correction_frames(2048 * 5) == 2);
    CHECK(d.correction_frames(10) == 1);

    Config tight;
    tight.max_drift_frames = 1;
    tight.codec = Codec::DTS;
    Encoder t(tight, pcm, spdif);
    CHECK(t.max_correction_frames() == 1);             // never below one sample

    if (failures == 0) std::puts("ok");
    return failures == 0 ? 0 : 1;
}
