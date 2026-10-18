# open-usb3hdcap

An open-source, user-space driver for the **StarTech USB3HDCAP** USB 3.0 capture card
(YUAN UB-530 design, USB ID `1164:F533`): HDMI, DVI, VGA, Component, Composite and S-Video,
with stereo audio.

Status: **early bring-up, first live capture working.** On macOS (Intel, 15.8) the tool captures
**HDMI 1280×720 at 60 fps with 48 kHz stereo audio** (115.7 MB/s, no packet errors), driven by a
replayed startup sequence. The demuxer has also been validated offline on VGA 640×480 and NTSC 480i
captures. Next: replace replayed sequences with real input/mode handling, then the OBS plugin.

## Layout
| Path | Contents |
|---|---|
| `include/hdcap.h`, `lib/` | `libhdcap`: device access (libusb), sequence runner, isochronous streaming, stream demuxer |
| `tools/hdcap-capture.c` | CLI: run sequences, stream, save PPM frames + WAV |
| `tools/pcap2seq.py` | Turn a USBPcap capture window into a sequence file |
| `sequences/` | Bootstrap sequence files (see `sequences/README.md`) |
| `tests/demux_file.c` | Feed a raw payload file through the demuxer |
| `docs/PROTOCOL.md` | The observed protocol: control requests, chips, stream format |

## Build (macOS or Linux)
Requires libusb ≥ 1.0.27 and pkg-config (`brew install libusb pkgconf`).

    make
    ./hdcap-capture -s sequences/open_hdmi.seq -t 5 -n 3 -o out   # HDMI source connected

## Clean-room note
This driver is written from protocol observations and public chip documentation, not from the
vendor driver's code. The vendor driver is not redistributed here.
