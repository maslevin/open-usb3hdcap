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
From the [Releases](https://github.com/maslevin/open-usb3hdcap/releases) page (current version: **0.3.0**):
- **USB3HDCAP-0.3.0.pkg**: camera, audio input and the background service. Double-click it,
  accept the notice, then open **USB3HDCAP** from Applications and click **Install Camera**. Approve
  it in System Settings → General → Login Items & Extensions. The app also picks the input and the
  deinterlacing mode. **After updating**, open the app and click **Install Camera** again: macOS keeps
  running the previous camera extension until the app asks it to replace it. To remove: in the app, click **Remove Camera**, then run
  `sudo "/Library/Application Support/open-usb3hdcap/uninstall.sh"`.
- **usb3hdcap-obs-0.3.0-macos-universal.zip**: the OBS plugin. Unzip it, quit OBS and run
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
| Audio | HDMI embedded, analog line-in; clock drift corrected (20-minute runs without glitches) | |
| Robustness | input switching, unplug and replug | |

### Help wanted: untested modes
These are implemented but **nobody has been able to test them yet**. If you have the hardware, please
open an issue with the **Test report** template. Working results are as valuable as failures.
- **PAL and SECAM** (576i) on Composite or S-Video, and 288p PAL consoles. The 576-line picture
  window is implemented from the decoder datasheet but never seen live.
- **Component HD**: 720p, 1080i, 1080p (and 576i/576p).
- **HDMI** modes other than 720p60: 1080i, 1080p, 576p, and sources using deep colour.
- **Apple Silicon Macs** (all binaries are universal but have only run on Intel), and **sleep and
  wake** while capturing.
- VGA above 1600×1200, and other VGA sources (older PCs, scan converters).
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

## Build from source
Requires libusb ≥ 1.0.27 and pkg-config (`brew install libusb pkgconf`).

    make
    ./hdcap-capture -i hdmi -t 5 -n 3 -o out      # inputs: hdmi dvi vga component composite s-video
    ./hdcap-capture -i vga -m "27 MHz" -H 0 -V 0 -o out   # force an analog mode, nudge the window
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
