# ac3spdif for Linux

Dolby Digital Live and DTS Connect for Linux: captures all system audio,
encodes it to AC-3 or DTS in real time, and bitstreams the result out an
S/PDIF or HDMI interface as an IEC 61937 data burst, so a receiver decodes 5.1
itself instead of being handed stereo PCM.

Windows sound cards have shipped this as "Dolby Digital Live" or "DTS Connect"
for two decades. PipeWire can *pass through* an AC-3 stream a player already
has, but nothing in the desktop will encode live system audio into one. This is
a port of the macOS [ac3spdif](https://github.com/) tool to Linux, with the
same pipeline and the same knobs, built on what Linux does better: the virtual
loopback device is created on demand instead of installed as a driver, the
system output switches over and back automatically, and the sound server can be
asked to hand over the hardware entirely.

Targets Fedora 44 first; builds and runs unchanged on Ubuntu 24.04+, Debian 13
and Arch/Manjaro. Anything running PipeWire 1.0 or later will do.

## Two ways to run it

**Tray app** (`ac3spdif-app`). Lives in the panel with no window. Start/stop,
pick devices and settings from the menu, and watch buffer, drift and underrun
figures live in the status window, which also holds the per-channel meters and
the latency slider. Quitting stops the stream and restores the default sink and
the output device, which is the point: it runs only while the app is open.

**Command line / background service** (`ac3spdif`), optionally installed as a
systemd user unit so it runs from login with no UI at all. The app's *Run as
Background Service* menu item writes and enables that unit for you, using the
settings currently selected.

The two are mutually exclusive by nature: the output sink accepts one exclusive
passthrough stream, so whichever starts first wins. The app checks for an
active service before starting and offers to stop it.

Both drive the same engine library, so there is one pipeline implementation.
`ac3spdif-probe` dumps everything PipeWire and ALSA know about the digital
outputs, for when something will not engage.

### The menu

| Item | What it does |
|---|---|
| **Start / Stop** | Begins or ends the bitstream |
| **Status Window…** | Live buffer, drift, underrun and latency figures, the channel meters, the latency slider, and the log. Middle-click on the tray icon opens it too |
| **Channel Activity** | Live per-channel level bars in dBFS, shown in the menu while streaming |
| **Meters** | Where the meters appear: in this menu, and optionally as one glyph per channel beside the panel icon |
| **Capture Source** | *Automatic* creates and captures the virtual sink; or any sink's monitor, or any source |
| **Output Device** | Where the bitstream goes. Each sink is annotated `✓ passthrough`, `✓ passthrough (enable)`, `~ PCM carrier` or `✗`, so the choice is informed rather than a guess from the name. *Automatic* picks the best digital sink. Below the separator, the ALSA devices for the direct transport |
| **Channels** | Capture width. `8-channel source → 5.1` is labelled that way because AC-3 and DTS have no 7.1 mode |
| **Codec** | Dolby Digital (AC-3) or DTS |
| **Bitrate** | Per codec; 640k is the most AC-3 allows, 1509k the most a DTS burst can hold |
| **Transport** | PipeWire passthrough, PCM carrier, or direct ALSA |
| **Latency** | Output ring size by name, or *Custom…* for the slider |
| **Self-Test Tones** | Replaces capture with a distinct tone per channel |
| **Route System Audio to the Encoder** | Make the virtual sink the default output while streaming |
| **Run as Background Service** | Installs and starts the systemd user unit |
| **Start Streaming When App Opens** | Auto-start, when the app itself is launched |
| **Launch App at Login** | An XDG autostart entry for the app |

### Channel activity meters

While streaming, the menu carries one row per channel going *into* the
encoder, labelled by surround position:

```
█████████████░░░░░░░  FL  −18 dB
████████████░▌░░░░░░  FR  −21 dB
```

Each bar is twenty segments of 3 dB from −60 dBFS, with a peak mark that falls
back over about a second, and a `!` beside a reading above −1 dB, where a
capture is about to clip. They meter the six channels feeding the encoder, not
the S/PDIF output: by the time audio reaches the output it is a bitstream, and
its "levels" would be the entropy of compressed data rather than anything
audible.

A tray menu on Linux is not a view but a list of text items exported over
D-Bus, so the bars are block glyphs rather than drawn, and they update ten
times a second by changing the item labels, which GNOME, KDE and the others
apply while the menu is open. The bar comes before the channel name so the bars
line up whatever the panel's proportional font makes of "LFE" against "FL".
The macOS app hides its meters behind ⌥ because they are diagnostics rather
than everyday controls; a tray menu cannot see modifier keys, so here they are
on while streaming and off with one switch in the **Meters** submenu. The same
submenu can put a compact meter beside the icon in the panel itself, one glyph
per channel in four shades on the same dB scale. The full drawn meters, with
red near full scale, live in the status window, a middle-click on the icon
away.

The menu is built once and then only ever updated in place. A tray menu is
exported over D-Bus with an id per item, and the panel caches that tree;
replacing the menu hands it a tree of new ids, which GNOME re-fetches and
rebuilds at idle priority, and a submenu open at that moment comes up empty.
Labels, check marks, visibility and sensitivity are all properties of an
existing item, so every setting change, the meter rows appearing while
streaming, and the bitrate list following the codec are property updates; the
menu is rebuilt only when the list of audio devices changes, checked every
half minute while idle.

A menu takes its width from its widest row, so a row whose text changes ten
times a second must never be that row, or the whole menu jitters with it. The
live figures (buffer, drift, underruns) therefore sit on their own short row
under the static "Streaming AC-3 to …" line, with numbers in fixed-width cells
and the changing one last; the meters section header carries invisible
trailing padding so it is always the widest row; every glyph in a bar or in the
panel meter is one that fonts draw at a single width. None of that shows, which
is the point.

### Settings

Every change in the menu or the status window is written at once to
`~/.config/ac3spdif/settings.ini`, so quitting, logging out or a reboot brings
the app back exactly as it was. The output buffer and the bitrate are kept per
codec, under `[ac3]` and `[dts]` in that file: a buffer at the AC-3 floor is
below the DTS floor, and the two bitrate lists do not even overlap, so one
shared value would be wrong for whichever codec was not in use when it was
chosen. Switching codec therefore switches to that codec's own settings, and
switching back finds the others untouched. The remaining settings (devices,
channels, transport, meters, auto-start) are shared.

Whether streaming resumes when the app opens is its own switch, *Start
Streaming When App Opens*, and *Launch App at Login* opens the app itself;
together they bring the stream back after a reboot without a click.

Changing any device or format setting while streaming restarts the engine,
since the configuration is fixed when the pipeline starts. That is deliberate:
leaving the menu showing one thing while the stream does another is worse than
a half-second gap.

## How it works

```
 all system audio
       │
       ▼
 virtual sink ──PipeWire──▶ pcmRing ──feed──▶ libavcodec ──▶ spdifRing ──▶ S/PDIF
 "ac3spdif"                            │       AC-3 / DTS       │         passthrough
 (created on start,                    │                        │         or direct ALSA
  default output)             drift correction         IEC 61937 framing
```

Four things make this work, and each is load-bearing:

**The virtual sink is created by the process and dies with it.** On start the
engine asks PipeWire for a null sink named `ac3spdif` with exactly the channel
layout being encoded, makes it the configured default output, and captures its
monitor. On stop, or if the process is killed, the sink vanishes and the
previous default comes back. There is no driver to install and nothing to
configure in a sound settings panel; applications simply see a 5.1 output
called "AC3SPDIF AC-3 encoder (5.1)".

**The output is a PipeWire passthrough stream, not PCM.** The stream offers
only the IEC 958 format for the selected codec. WirePlumber links it in
passthrough mode, the ALSA sink switches its hardware format, raises the IEC
60958 channel-status *non-audio* bit that tells a receiver to route the stream
to its Dolby or DTS decoder rather than its DACs, and nothing between the
stream and the wire can mix, resample or scale the burst. The link is
exclusive: while it exists the sink accepts nothing else, which is what hog
mode was for on macOS.

**libavformat's `spdif` muxer does the IEC 61937 framing**, in-process through
libavcodec. Encoding is only half the job; the bytes still need the burst
preamble. Each AC-3 frame becomes exactly 6144 bytes: `Pa=0xF872, Pb=0x4E1F`,
`Pc` carrying the data type, `Pd` the payload length in bits, then the frame
byte-swapped for 16-bit transport and zero-padded to the burst. This is the
same code the `ffmpeg -f spdif` command line runs, so a `--dump` file decodes
straight back with ffmpeg.

**Clock drift is corrected in the PCM domain.** When capture and output are
clocked by different oscillators, one is always slightly faster. A normal audio
path fixes that by resampling, but a bitstream cannot be resampled: every byte
of a burst is load-bearing. So the feed thread always hands the encoder exactly
one frame's worth of samples while varying how many it *consumes* to build it:
consume 1537 and emit 1536 to shed a sample, consume 1535 and repeat the last
one to add one. The correction is proportional to how far the output ring sits
from its target, capped by `--max-drift`. On PipeWire the two streams are also
put in one link group, so where possible the graph clocks the virtual sink from
the output device and the controller has nothing to do; measured here it
settles at a few ppm.

## Requirements

- PipeWire 1.0 or later with WirePlumber (Fedora 34+, Ubuntu 22.10+, Debian 12+,
  Arch/Manjaro). The direct ALSA transport also works with no sound server at all.
- A digital output: an S/PDIF (optical or coaxial) port, or HDMI. On HDMI the
  display or receiver must accept the codec; PipeWire reads that from its EDID.
- ffmpeg's libraries as shipped by the distribution. Fedora's `ffmpeg-free`
  in the standard repositories includes the AC-3 and DTS encoders and the
  `spdif` muxer; RPM Fusion is not needed.
- For the tray icon on GNOME, the AppIndicator extension
  (`gnome-shell-extension-appindicator`). KDE Plasma, XFCE, MATE and Cinnamon
  show it natively.

## Building

```sh
./scripts/install-deps.sh   # dnf, apt or pacman, from the standard repositories
./scripts/build.sh          # CMake + Ninja into ./build, then the unit tests
./scripts/install.sh        # optional: binaries, icons and launcher into ~/.local
```

`install-deps.sh` needs a C++20 compiler and the development packages for
libpipewire, alsa-lib, libavcodec/libavformat/libavutil/libswresample, GLib,
GTK 3 and libappindicator (Ayatana on Debian and Ubuntu). `BUILD_APP=OFF
./scripts/build.sh` skips the tray app and the GTK dependency for a headless
box.

### Packages

`scripts/package.sh` builds installable packages into `dist/`, each in a clean
environment so the result depends only on what the package declares:

```sh
./scripts/package.sh rpm       # Fedora:        dist/ac3spdif-0.1.0-1.fc44.x86_64.rpm
./scripts/package.sh deb       # Ubuntu/Debian: dist/ac3spdif_0.1.0_amd64.deb
./scripts/package.sh flatpak   # any distro:    dist/org.ac3spdif.App.flatpak

sudo dnf install ./dist/ac3spdif-*.rpm
sudo apt install ./dist/ac3spdif_*.deb
flatpak install --user ./dist/org.ac3spdif.App.flatpak
```

The RPM and the deb are built in Fedora and Ubuntu containers, so only podman
is needed on the host (`FEDORA=45` or `UBUNTU=22.04` target another release).
Both declare every library they link, so `dnf` and `apt` install ffmpeg's
libraries, PipeWire, ALSA, GLib, GTK and the appindicator alongside; nothing
beyond the standard repositories is needed. The Flatpak is built with
`org.flatpak.Builder` from Flathub against the freedesktop 26.08 runtime,
whose ffmpeg carries the AC-3 and DTS encoders and the IEC 61937 muxer, and
builds the appindicator stack in, so the bundle is self-contained. The
packaging lives in `packaging/fedora`, `packaging/debian` and
`packaging/flatpak`.

Inside the Flatpak the command line is
`flatpak run --command=ac3spdif org.ac3spdif.App …` and the probe is
`--command=ac3spdif-probe`. The sandbox has the PipeWire socket, the sound
devices for the direct ALSA transport, the D-Bus names for the tray icon and
the device reservation protocol, and what service mode needs (below). A
`--dump` file has to land somewhere the sandbox can write, such as
`~/.var/app/org.ac3spdif.App/`, unless you add `--filesystem=home` to the run.

## Use

```sh
# What can carry a bitstream, and which sink is picked automatically.
./build/ac3spdif --list

# Verify the receiver end first: a distinct tone per channel. Your receiver
# should display "Dolby Digital" and each speaker should sound a different note.
./build/ac3spdif --selftest

# Run it. The virtual sink appears as the default output; play anything.
./build/ac3spdif

# DTS instead, at the lowest buffer its floor allows.
./build/ac3spdif --codec dts --prebuffer 4

# Stereo, to a named sink.
./build/ac3spdif --channels 2 --output alsa_output.usb-Foo-00.iec958-stereo

# Direct ALSA, taking the card away from the desktop for the lowest latency.
./build/ac3spdif --output alsa:iec958:CARD=PCH,DEV=0

# Capture a copy of exactly what went to the wire, and decode it back.
./build/ac3spdif --dump /tmp/out.spdif
ffmpeg -f spdif -i /tmp/out.spdif /tmp/out.wav
```

Ctrl-C restores the default sink and releases the output. The same happens on
SIGTERM, so a logout or `systemctl --user stop` is clean too. If the process is
killed outright, the virtual sink still disappears, because PipeWire owns it on
the process's behalf; only the configured default sink may need putting back by
hand in Sound settings.

### Stereo sources

Most applications hand PipeWire stereo. They still get a Dolby or DTS
bitstream, a 2.0 one, which every receiver decodes. Whether stereo content gets
spread across the surround speakers is the receiver's upmixer's business
(Dolby Surround, Pro Logic II, Neo:6), a setting on the receiver rather than in
the bitstream. mpv, VLC, Kodi, Firefox and Chromium all emit discrete 5.1 to a
5.1 output when the content has it.

## Transports

**passthrough** (default) is the one to use wherever it is offered. A sink that
does not list the codec yet, common for S/PDIF ports, has it enabled on the way
in: the engine writes the IEC 958 codec list through the device route, which
WirePlumber persists, so `--list` shows `✓ passthrough` from then on. HDMI sinks
list what the display's EDID allows and are used as they are.

**alsa** opens the ALSA device directly. First it asks WirePlumber to release
the card over D-Bus (the `org.freedesktop.ReserveDevice1` protocol JACK has
used for years), then opens the `iec958:` or `hdmi:` PCM with the AES
channel-status arguments that set the non-audio bit, and writes bursts straight
into the kernel's ring with periods a few milliseconds long. The whole card
leaves the desktop while streaming, including any capture device on it, and
WirePlumber takes it back the moment the process exits. Measured here it takes
about 100 ms to change hands each way.

**pcm** writes the same bytes into an ordinary 16-bit stereo stream. It only
survives if the volume is unity end to end and nothing resamples, so the sink's
levels are forced for the run and put back afterwards, and the stream is marked
to skip remixing and resampling. The non-audio bit is never set, so the receiver
has to spot the burst preamble by itself, which is how DTS audio CDs have always
worked. Strictly a fallback for a sink that will not take a passthrough stream.

**Never listen on an analog output while this is running.** A burst
interpreted as PCM is full-scale digital noise. Automatic selection only ever
picks a sink whose port reports S/PDIF or HDMI; an analog sink has to be named
deliberately, and the engine warns when it is.

## Latency

The status output prints a breakdown once per run. Every term is a real
reading, a negotiated quantum or a current ring level, not an estimate:

```
latency : capture               128 frames     2.7 ms   stream quantum plus reported delay
latency : pcm queue               0 frames     0.0 ms   transient
latency : AC-3 frame           1536 frames    32.0 ms   inherent to the format
latency : output ring          2048 frames    42.7 ms   what --prebuffer controls
latency : device               1024 frames    21.3 ms   reported by the output
latency : TOTAL                4736 frames    98.7 ms   end to end
```

The output ring is the one knob worth turning. `--latency` picks a size by
name, `--prebuffer` sets it in bursts directly, and the app has a slider in
whole bursts, because a burst is one encoded frame and trimming a partial one
would destroy the alignment the receiver locks onto.

| `--latency` | bursts | AC-3 | DTS |
|---|---|---|---|
| minimum | 2 | 64 ms | 21 ms |
| low | 3 | 96 ms | 32 ms |
| balanced | 4 | 128 ms | 43 ms |
| safe (default) | 6 | 192 ms | 64 ms |
| maximum | 12 | 384 ms | 128 ms |

How low you can usefully go depends on the device and the codec. Two effects
set the floor and the larger wins: the encoder hands over a whole burst at a
time while the device drains continuously, which costs a spare burst whatever
the hardware does; and the ring has to cover about 40 ms of scheduling jitter,
which is 2 AC-3 bursts but 4 DTS bursts. The engine warns at start-up when the
buffer is below the floor it works out for the device in use, and the slider
turns red. Neither forbids it.

Measured on this machine (Fedora 44, PipeWire 1.6.9, Intel HDA S/PDIF), with
the self-test source and every underrun counted:

| Codec | Transport | `--prebuffer` | Total | Underruns |
|---|---|---|---|---|
| AC-3 | passthrough | 6 | 229 ms | 0 |
| AC-3 | passthrough | 2 | 99 ms | 0 |
| DTS | passthrough | 6 | 91 ms | 0 |
| DTS | passthrough | 4 | 67 ms | 0 |
| DTS | direct ALSA, 256-frame buffer | 4 | 59 ms | 0 |
| DTS | direct ALSA, 128-frame buffer | 2 | 37 ms | **climbing** |

The last row is the floor being real: two DTS bursts is 21 ms of cover, the
drift controller pins at its ceiling trying to catch up, and the ring runs dry.
Four is the smallest that holds, exactly as computed.

**The device term.** On the passthrough transport PipeWire's ALSA sink keeps
its own quantum of 1024 frames (21 ms) once it is in IEC 958 mode, whatever the
stream asks for; `--device-buffer` and even a forced graph quantum do not move
it. That term is the price of the sound server, and the direct ALSA transport
is how to get below it: 3 to 6 ms with a 128 or 256 frame buffer.

Everything else is fixed by the format: an AC-3 frame is 1536 samples whatever
you do, and the encoder cannot emit it until it has all of them. DTS frames are
a third the length, and the whole pipeline shortens with them.

## Codecs

`--codec ac3` (default) or `--codec dts`, and a **Codec** submenu in the app.

| | AC-3 | DTS |
|---|---|---|
| Frame | 1536 samples | 512 samples |
| Burst | 6144 bytes | 2048 bytes |
| Burst duration at 48 kHz | 32 ms | **10.7 ms** |
| IEC 61937 data type | 1 | 11 |
| Default bitrate | 640k | 1509k |
| Offered bitrates | 192k–640k | 754k–1509k |
| Encoder | libavcodec `ac3` | libavcodec `dca` (experimental) |

The bitrates do not overlap. The DTS encoder refuses anything at or below 640k
for 5.1, which is every AC-3 setting, and fails to open rather than degrading.
A bitrate the codec cannot take is replaced with that codec's default and the
substitution is logged; the app's bitrate menu only offers what the selected
codec accepts, and switching codec clears a stale value.

AC-3 stops at 640k because A/52 does. DTS stops at 1509k because a Type I burst
is 2048 bytes with an 8-byte preamble; 1536k needs all 2048 and the muxer then
emits a stream with no sync pattern in it, not an error, just silently
unusable. 1411k, the rate DTS discs carry, is the safer choice for a receiver
of unknown temperament.

**5.1 is the ceiling, and it is the codecs' ceiling.** Neither has a 7.1 mode;
that arrived with E-AC-3 and DTS-HD, which S/PDIF does not carry. Selecting the
8-channel capture is still worthwhile when the source emits eight channels,
because the side pair is folded into the back pair at −3 dB instead of being
dropped; but what reaches the receiver is 5.1 either way, and the UI says so.

## Diagnosing a silent speaker

```sh
./build/ac3spdif --output none --meters
```

`--meters` prints a level bar per captured channel, labelled by position, and
a summary on exit of the peak each channel saw over the whole run, which is
what answers the question after playback has stopped. `--output none` encodes
and throws the bitstream away, pacing the discard at the wire rate, so capture
can be examined while something else is using the hardware.

If no channel saw anything, the virtual sink is not receiving system audio:
check Sound settings shows it as the output, and note that an application that
was already playing may stay bound to the device it started on; restart
playback after switching. If the levels are right but the receiver is silent,
`--dump` the stream and decode it with `ffmpeg -f spdif`: if the file decodes
to the right audio on the right channels, everything upstream of the cable is
correct. `ac3spdif-probe` shows the codec list, port type and route
availability of every sink, and the IEC 958 status bytes of every ALSA digital
output.

## Service mode

```sh
./scripts/install-service.sh --codec dts --latency low   # any ac3spdif arguments
journalctl --user -u ac3spdif -f
./scripts/install-service.sh uninstall
```

A user unit, not a system service: the sink switch and the output device
belong to the logged-in session. The app's *Run as Background Service* writes
the same unit with the current settings, and *Launch App at Login* is the
other thing, an autostart entry for the app itself. Only the service survives a
reboot on its own.

The app manages the unit through systemd's D-Bus API and writes it, like the
autostart entry, to the host's config directory, so both also work from inside
the Flatpak, where the unit runs `flatpak run --command=ac3spdif
org.ac3spdif.App …` with the chosen settings.

## Verified

End to end on an Intel HDA ALC892 S/PDIF output, Fedora 44, PipeWire 1.6.9:

- Six tones at distinct levels played into the virtual sink as 5.1 came back
  from a decoded `--dump` at exactly their levels on their own channels: FL
  0.100, FR 0.150, FC 0.200, LFE 0.250, RL 0.300, RR 0.350.
- The sink negotiated `iec958: AC3, 48000 Hz` and the port's channel status
  read *non-audio, 48000 Hz, PCM coder* while streaming and returned to
  *audio, 44100 Hz* on exit.
- The default output moved to the virtual sink for the run and back afterwards.
- DTS negotiated the same way at 1509k, 94 bursts a second.
- The direct ALSA transport took the card from WirePlumber in about 100 ms,
  streamed with 128-frame periods, and handed it back on exit.
- The framing test encodes a second of audio through both codecs and checks
  every burst's preamble, data type and length; it runs under `ctest`.

Not yet verified, for want of hardware rather than effort: a receiver's front
panel saying "Dolby Digital", and HDMI output to a receiver rather than a
television. Everything upstream of the cable is confirmed correct.

## Things worth knowing

- **GNOME needs the AppIndicator extension** for the tray icon. Without it the
  app still runs and shows its status window instead.
- **PipeWire may rename a sink** with a `.N` suffix when it is recreated while
  an old instance lingers, which the direct ALSA transport causes by design.
  Saved names are matched with that suffix ignored.
- **PulseAudio-only systems** (without PipeWire) are not supported by the
  passthrough transport; the direct ALSA transport works there for output, but
  capture needs PipeWire.
- **The desktop volume applies.** The virtual sink's volume is applied to what
  the encoder receives, so the volume keys behave as they do with any output.
  Keep it at 100% for full encoder headroom and use the receiver's volume.

## Licence

This project is MIT licensed; see `LICENSE`. It links dynamically against the
ffmpeg libraries shipped by your distribution (libavcodec, libavformat,
libavutil, libswresample), which are LGPL-2.1+ or GPL depending on how the
distribution built them, and against libpipewire, alsa-lib, GLib, GTK and
libappindicator under their own licences. Nothing from ffmpeg is bundled.
