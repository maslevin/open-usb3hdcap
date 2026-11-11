# open-usb3hdcap

An open-source driver for the **StarTech USB3HDCAP** USB 3.0 capture card (YUAN UB-530 design, USB ID
`1164:F533`): HDMI, DVI, VGA, Component, Composite and S-Video, with stereo audio. The vendor only
supports Windows; this project makes the card work on **macOS** without any vendor software, and its
core library is portable C built on libusb.

## What you get on macOS
- **A camera** named "USB3HDCAP" that any app can use: QuickTime, FaceTime, Zoom, browsers, OBS.
  It uses a CoreMediaIO camera extension, with optional deinterlacing for VHS, 480i and 1080i sources.
- **An audio input** named "USB3HDCAP" (Core Audio plug-in), with HDMI embedded audio or the analog
  line-in.
- **An OBS Studio plugin** ("USB3HDCAP Capture") with full control of input, timing and picture.

Requirements: macOS 13 or later (Intel or Apple Silicon universal binaries), and a **USB 3** port and
cable. On USB 2.0 only Composite and S-Video fit.

## Install
From the [Releases](https://github.com/maslevin/open-usb3hdcap/releases) page (current version: **0.3.1**):
- **USB3HDCAP-0.3.1.pkg**: camera, audio input and the background service. Double-click it,
  accept the notice, then open **USB3HDCAP** from Applications and click **Install Camera**. Approve
  it in System Settings → General → Login Items & Extensions. The app also picks the input and the
  deinterlacing mode. **After updating**, open the app and click **Install Camera** again: macOS keeps
  running the previous camera extension until the app asks it to replace it. To remove: in the app, click **Remove Camera**, then run
  `sudo "/Library/Application Support/open-usb3hdcap/uninstall.sh"`.
- **usb3hdcap-obs-0.3.1-macos-universal.zip**: the OBS plugin. Unzip it, quit OBS and run
  `./install.sh` (see `INSTALL.md` inside).

Both are signed with a Developer ID and notarized by Apple. Use one at a time: the camera/audio
service and the OBS plugin cannot use the card simultaneously.

## Tested hardware
Verified live on an Intel Mac (macOS 15), with OBS 32.2.2, QuickTime and ffmpeg:

| Input | Verified | Sources used |
|---|---|---|
| HDMI | 1280×720p60, embedded 48 kHz audio | Sega Genesis Mini |
| DVI-D | 800×600 to 1920×1080p60 (VESA and CEA timings), RGB and YCbCr | PC (AMD Radeon HD 7000) |
| VGA | 640×480 to 1600×1200 @ 60 Hz: 800×600, 1024×768, 1152×864, 1280×720, 1280×1024, 1360×768, 1440×900, 1600×900, 1600×1200 | PC through an active VGA converter; Sega Dreamcast VGA box (incl. its 27 MHz 640×480) |
| Component | 480i, 480p | DVD player |
| S-Video | NTSC 480i, picture controls | DVD player |
| Composite | NTSC 480i (deinterlaced to 60 fps in the camera), **240p** (line-doubled to 720×480 at 60 fps) | VHS deck, Sega Dreamcast, Super Nintendo |
| VGA, 15 kHz RGB | **240p** (60 Hz) and **288p** (50 Hz), detected automatically and line-doubled to 720×480 / 720×576 (4:3): Genesis H40 (320×224), Amiga NTSC hires (640×200) and PAL lores; 200-line modes up to 1200×200, 1200×400 interlaced, switching between modes while capturing. OBS plugin and `hdcap-capture` on an Apple Silicon Mac (M1 Max, macOS 26.6.2) | MiSTer FPGA analog VGA output with `vga_scaler=0`, `forced_scandoubler=0` (Genesis core with the 240p Test Suite, Amiga core) |
| VGA, Sharp X68000 | **31 kHz** 768×512 (Human68k), 512×512 and 384×512, all sampled at the core's pixel clock (1 or 2 samples per pixel); **24 kHz** (Marble Madness, 640×448 window); **15 kHz** 256×256 (Space Harrier, via the 240p mode). Automatic sampling phase, steady text; switching between cores and back. OBS plugin and `hdcap-capture`, Apple Silicon Mac | MiSTer FPGA X68000 core, same output settings |
| VGA, 25 kHz arcade | Atari System 2 medium resolution, 512×384 at 59.94 Hz, sampled at its pixel clock (OBS, `hdcap-capture`) | MiSTer FPGA 720 Degrees core |
| Audio | HDMI embedded, analog line-in; clock drift corrected (20-minute runs without glitches) | |
| Robustness | input switching, unplug and replug | |

### Help wanted: untested modes
These are implemented but **nobody has been able to test them yet**. If you have the hardware, please
open an issue with the **Test report** template. Working results are as valuable as failures.
- **PAL and SECAM** (576i) on Composite or S-Video, and 288p PAL consoles. The 576-line picture
  window is implemented from the decoder datasheet but never seen live.
- **Component HD**: 720p, 1080i, 1080p (and 576i/576p).
- **HDMI** modes other than 720p60: 1080i, 1080p, 576p, and sources using deep colour.
- **Apple Silicon Macs**: the OBS plugin and `hdcap-capture` have run on an M1 Max (macOS 26.6.2);
  the camera/audio service has not. **Sleep and wake** while capturing.
- VGA above 1600×1200, and other VGA sources (older PCs, scan converters).
- 15 kHz RGB beyond what is listed above: other computers, consoles and arcade boards, 576i, the
  camera/audio service with 15 kHz sources, and **240p/288p on Component** (same modes and line
  doubling as VGA, but never seen live).
- A **real Sharp X68000** (not the MiSTer core): it runs 31 kHz at about 55 Hz and 31.5 kHz, and
  its 512-dot modes use a slower pixel clock, so it needs modes of its own. Interlaced X68000
  modes have not been tried on the core either.
- Automatic sampling phase on PC VGA sources at their native resolutions.
- Other StarTech/YUAN models built on the same chips (for example the StarTech XCAPTURE-1).
- Linux: the core library and `hdcap-capture` build there, but haven't been run.

### Known limitations
- 1600×900@60 and 1280×960@60 have identical analog timings. Auto picks 1600×900; choose 1280×960
  manually (OBS: Analog timing).
- **HDCP:** the driver does not accept HDCP-protected content. The capture chip's HDCP receiver is
  switched off, so sources withhold protected video (they typically show their own HDCP error
  instead); if HDCP is nonetheless reported active, the capture shows an "HDCP-protected source /
  capture blocked" notice and audio is muted. Unprotected content from the same device (for example
  a streaming box's menus) captures normally.
- Composite/S-Video NTSC frames are 720×478, two lines fewer than the Windows driver delivers.
- 15 kHz RGB (240p/288p) is sampled at the standard 13.5 MHz, 720 samples per line, not at the
  source's own pixel clock: about 2 samples per Genesis H40 pixel, slightly under 1 per Amiga hires
  pixel. Vertical edges can be a sample soft, and the finest horizontal detail of hires modes is
  not fully resolved. The picture position was tuned on a MiSTer FPGA (Genesis and Amiga cores);
  other sources may sit a few samples off centre (adjust with the horizontal offset). Interlaced
  15 kHz sources rely on the receiver's interlace flag; if it read a 480i or 576i source as
  progressive, each field would be shown line-doubled instead of woven.
- **Sampling phase (VGA/Component):** for modes sampled at the source's own pixel clock (PC/VESA
  modes, the X68000 modes) the driver picks the sharpest sampling phase with the capture chip's
  sharpness meter. The first time such a mode appears this takes about 1.2 s before the picture
  starts; after that the mode's phase is remembered (until OBS or the service restarts) and set at
  once. The phase is checked again 3 and 10 s after a start (holding the last frame for a sixth of
  a second), and for two minutes the driver watches how much detail the picture has, which doesn't
  disturb it: a picture with much more detail than the one the phase was found on (a desktop after
  a boot screen) is measured again, holding the last frame for about a second. A picture with less
  detail never changes the phase. Broadcast-rate modes (240p, 288p, 480i/p, 576i/p, HD) keep the
  phase the input tables set, as before: they sample at 13.5 MHz (or the HD rates) whatever the
  source's pixel clock, so there is no right phase, and on undersampled detail such as Amiga hires
  the meter preferred the phase that flickered most.
- The X68000 and Atari System 2 modes match the **MiSTer FPGA cores'** timing; the original
  machines may differ slightly. The X68000's 768×512 line is a fraction of a sample longer or
  shorter than the 1104 samples the chip can be set to, so even at the best phase
  about 0.1 % of the pixels of a static screen are not perfectly steady. The 24 kHz window is 640
  wide; software using a wider picture would be cropped (adjust the horizontal offset).

## Build from source
Requires libusb ≥ 1.0.27 and pkg-config (`brew install libusb pkgconf`).

    make
    ./hdcap-capture -i hdmi -t 5 -n 3 -o out      # inputs: hdmi dvi vga component composite s-video
    ./hdcap-capture -i vga -m "27 MHz" -H 0 -V 0 -o out   # force an analog mode, nudge the window
    ./hdcap-capture -i vga -p 25 -o out           # force the ADC sampling phase (0-63; default automatic)
    ./hdcap-capture -i vga -T 1104,768,263,512,41 -o out   # try a custom timing: samples per line, window
                                                           # width, start, height, first line
    ./hdcap-capture -i composite -P -9,121,128,0,2 -o out  # picture: brightness, contrast, saturation, hue, sharpness

- **OBS plugin, development build** (OBS 28+): `brew install cmake simde`, then
  `INC=$(obs-plugin/fetch-libobs-headers.sh)`, `cmake -S obs-plugin -B build -DLIBOBS_INCLUDE_DIR="$INC"`
  and `cmake --build build && cmake --install build`.
- **OBS plugin package**: `packaging/package-macos.sh`. This builds a universal build with bundled
  libusb, ad-hoc signed; set `SIGN_IDENTITY` and `NOTARY_PROFILE` for a signed and notarized build.
- **macOS camera, audio and service**: `make -C macos`. For signed builds and the installer
  (`make -C macos pkg`), see [docs/MACOS_DRIVERS.md](docs/MACOS_DRIVERS.md) and
  `macos/signing.mk.example`.

## Layout
| Path | Contents |
|---|---|
| `include/hdcap.h`, `lib/` | `libhdcap`: device access (libusb), input selection, signal detection, analog timing modes, isochronous streaming, demuxer, capture session, audio drift resampler, deinterlacer |
| `obs-plugin/` | OBS Studio source plugin |
| `macos/` | `hdcapd` service, camera extension and host app, Core Audio plug-in, installer ([docs/MACOS_DRIVERS.md](docs/MACOS_DRIVERS.md)) |
| `tools/` | `hdcap-capture` CLI; `gen_tables.py` / `pcap2seq.py` (register tables and sequences from USB captures) |
| `tests/` | Offline tests: decoding of captured register values, demuxer, resampler, deinterlacer |
| `docs/PROTOCOL.md` | The protocol as observed: control requests, chips and registers, stream format |

## How this was made
The card's protocol was worked out for interoperability. We recorded the Windows driver's USB traffic
for each input and mode, and analysed the Windows driver binary to understand the purpose of what it
sends. The driver itself is written from those observations and the public datasheets of the chips
(TW9900, CS53L21). **No vendor code, firmware or datasheet text is included**, and the card needs no
firmware from the host. Some register details were cross-checked against two open-source Linux drivers,
credited in [docs/PROTOCOL.md](docs/PROTOCOL.md):
- [stoth68000/hdcapm](https://github.com/stoth68000/hdcapm) (same receiver chip)
- [mlaux/usb3hdcap](https://github.com/mlaux/usb3hdcap) (same card)

No code was copied from either.

## License
Copyright (C) 2026 Matt Slevinsky.

open-usb3hdcap is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 2 of the License,
or (at your option) any later version (`SPDX-License-Identifier: GPL-2.0-or-later`). See
[LICENSE](LICENSE). Release packages bundle [libusb](https://libusb.info) (LGPL-2.1-or-later).

## Disclaimer
This is an independent project, not made, endorsed or supported by StarTech.com, YUAN High-Tech or any
chip vendor; trademarks belong to their owners. The software is provided **as is, without warranty of
any kind**. Islandia Logic Ltd. distributes the signed macOS binaries free of charge as a convenience
only, with no warranty, support obligation or liability, to the fullest extent permitted by law. See
[NOTICE](NOTICE) for the full terms (also shown by the installer) and [LICENSE](LICENSE).
