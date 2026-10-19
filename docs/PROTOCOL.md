# USB3HDCAP USB protocol (observed)

This file states facts observed on the wire (USBPcap on a Windows 10 PC). It is meant to be the
clean-room input for an open driver, so it deliberately contains no vendor code.
Evidence for each fact is in [LOG.md](LOG.md); sources are `captures/c0*.pcap`.

Status: **draft**. Register meanings are not decoded yet. Sequences are recorded as observed.

## Device
- USB 3.0 SuperSpeed, VID:PID `1164:F533`, bcdDevice `0x1B5B`. One configuration; vendor class `FF/00/00`.
- Enumerates directly with final IDs. **No firmware download** at any point after a physical
  replug (c05, 00:11:17 UTC): only standard descriptor requests, SET_CONFIGURATION, then vendor requests.

| Interface | Alt | Endpoint | Type | Size | Use |
|---|---|---|---|---|---|
| 0 | 0 | 0x83 IN | Isochronous | 0 | stream off |
| 0 | 1 | 0x83 IN | Isochronous | 1024 × burst 16, Mult 0 (16 KiB / 125 µs) | **stream on (used by the Windows driver)** |
| 0 | 2 | 0x83 IN | Isochronous | 1024 × burst 16, Mult 1 (32 KiB / 125 µs) | not observed in use |
| 1 | 0 | 0x81 IN | Interrupt | 64, bInterval 7 | no traffic observed so far |

## Vendor control requests (bmRequestType 0x40 OUT / 0xC0 IN, recipient device)

Semantics combine wire observations with the decompiled driver (behavior only). "Seen" = observed on F533.

| bRequest | Dir | wValue | wIndex | Data | Meaning | Seen |
|---|---|---|---|---|---|---|
| **0xC0** | OUT/IN | I2C addr (8-bit) | register | 1 B | **I2C register write/read** | ✓ |
| 0xC0 | OUT/IN | I2C addr \| **0x5000** | 0 | n B | I2C **block** write/read (no register byte; used for the MCU at 0x6C) | — |
| **0xC1** | OUT | `(op << 8) \| pin` | 0 | — | **FX3 GPIO control.** Ops seen in the driver: 0x00, 0x01, 0x40, 0x80, 0xC0. On F533 only pin **0x2D (45)**: `40, 01, (50 ms) 00, (50 ms) 01, 80`, a **50 ms low reset pulse**, immediately followed by MST3367 reg 0x51 ← 0x80. GPIO 45 is most likely the MST3367 reset line | ✓ |
| 0xC1 | IN | pin | 0 | 1 B | GPIO read | — |
| **0xC2** | OUT | 0 | 0 | — | Sent once at open (purpose not named in the driver). There is also an IN form returning 1 byte | ✓ |
| **0xC7** | OUT | 100 | 0 | — | Sent once at open. Success sets a capability flag (bit 3) | ✓ |
| **0xC5** | OUT | 0 | 0 | — | **Capability probe**: the driver only records success (flag bit 1). **Stalls on F533 → feature absent** | ✓ (stall) |
| **0xC6** | OUT | 0 | 0 | — | **Capability probe** (flag bit 2). Succeeds on F533 | ✓ |
| 0xC3 | OUT/IN | val | idx | 0/2 B | Register-style access (sibling products) | — |
| 0xEC | OUT | — | — | — | Sibling-product init path | — |
| 0xA0 | OUT/IN | addr low 16 | addr high 16 | n B | **Cypress RAM upload/download** (standard FX bootloader request). Present in the driver, **never used on F533** | — |

Open sequence (driver init, matches c05): `C2(0)`, `C7(100)`, `C5` probe, `C6` probe, then the GPIO-45 reset
pulse + `9C:51=80`, then 0x98 setup and chip init.

## Board (from photos, `photos/IMG_4903–4912`)
PCB silkscreen **UB-530ST REV:1.1** (YUAN UB-530 design), made in Taiwan.

| Part (marking) | Function | I2C (8-bit) | Confidence |
|---|---|---|---|
| MStar **MST3367CMK-LF-170** (date 1552) | HDMI receiver + triple ADC (VGA/Component) | 0x9C | Marking clear |
| Altera **MAX II EPM570T144C5N** CPLD | Board glue/router logic | **0x98** (inferred: register behavior, a `'T'` signature register, no other candidate part) | Marking clear; address mapping inferred |
| Cirrus Logic **CS53L21** ("53L21C") | Stereo audio ADC with PGA | **0x94** (the CS53L21's I2C address) | Marking clear; matches driver gain logic |
| **Techwell TW9900** ("0BTA1-GR", date 1542; 32-pin QFP) | NTSC/PAL composite/S-Video decoder | **0x88** | Marking clear (IMG_4913) |
| Unmarked/covered square package near the USB 3.0 connector | Most likely the Cypress CYUSB3014 (FX3) | — | Inferred |
| Several SOIC/TSSOP-8 parts, one 16-pin SSOP near the VGA/DVI connector | Regulators, serial flash/EEPROM, an analog switch (likely) | — | Markings not readable |

No auxiliary MCU (I2C 0x6C) is identifiable, and the driver never accessed 0x6C on this unit.

## I2C devices behind the FX3

| 8-bit addr | Identified as | Evidence |
|---|---|---|
| 0x9C | MStar MST3367 (HDMI receiver / VGA ADC) | Driver strings; bank select at reg 0x00 (banks 0, 1, 2, 0x80 seen); mode-detect polling |
| 0x88 | Composite/S-Video video decoder. **Techwell TW9900** (marking read from board photo IMG_4913) | Polled while an analog SD input is selected (status regs 0x01, 0x1C, plus 0x30/0x4F/0x53/0x54). **Input select: reg 0x02 = 0x40 / reg 0x06 = 0x42 for Composite; reg 0x02 = 0x54 / reg 0x06 = 0x40 for S-Video** (c13, the only differences between the two 72-write reconfiguration bursts) |
| 0x98 | **Altera MAX II CPLD (EPM570), YUAN glue logic** (inferred from the board + behavior; driver code checks reg 0xF0 == `'T'` as a signature on some sibling models, which hints at a CPLD/FPGA). Driver-derived: **reg 0x00** = status with cable-detect bits (bit 4 DP, bit 2 HDMI, bit 3 VGA; active-low); **reg 0x20** = control (bit 2 audio enable, cleared on "AUDIO OFF"; bit 6 set when the active field height is 240/288, i.e. SD interlaced); reg 0x10 = 0xFE/0xFC by board variant; reg 0x3B written 0x80/0x02 by mode. The plug-detect and signature paths are not exercised on F533 | Written at every input change. SD decoder path (Composite/S-Video): `20=05, 00=01, 10=FE, 20=75`. MST3367 path (Component, and VGA/HDMI in c05): `20=05, 00=01, 10=FE, 01=03, 11=FC (×2), 20=15, 20=14, 20=15`. **Reg 0x20 upper nibble 7 vs 1 tracks SD-decoder vs MST3367 source.** Reg 0x01 = 02/03 seen (HDMI vs analog? TBD) |
| 0x94 | **Cirrus Logic CS53L21** stereo audio ADC (identified from the board). **Programmable gain:** Driver-derived: per-channel volume clamped to a signed 7-bit range (−0x67…+0x18 around a center), written to regs 0x0A–0x11 depending on a mode; reg 0x03 bit 0 selects the layout. Register meanings can now be checked against the public CS53L21 datasheet | The same 16-register write block before every stream start: `03=4e 04=44 07=00 0a..0d=00 1c=c0 04=46 09=42 0e=05 0f=05 18=00 ff=ff 0e=05 0f=05` |
| 0x6C | Aux MCU (from driver analysis) | **No traffic observed** in normal operation |

## Stream
- Started with SET_INTERFACE(IF0, alt 1) and stopped with SET_INTERFACE(IF0, alt 0).
- The Windows driver queues URBs of 32 isochronous packets. Each 125 µs packet slot is either
  **16384 bytes or empty**, and the device paces a constant rate (no gaps at frame boundaries).
- Concatenating the non-empty packet payloads gives a byte stream of **tagged records**. Packet
  boundaries do not align with records. Verified on c06 (300 MB, 2 resyncs):

| Record | Bytes | Meaning |
|---|---|---|
| `FF 00 00 XY` + 2·width | video line | packed **YUYV (YUY2)**, BT.601 levels (black Y=0x10). `XY` is the **BT.656 status byte**: **0x80** field 1 active, **0xAB** field 1 V-blank, **0xC7** field 2 active, **0xEC** field 2 V-blank. Progressive sources only use 0x80/0xAB |
| `FF 00 FF N` + 4·N | audio | **N stereo frames, 16-bit LE (L, R)**. N = 3 or 2 (mostly 3) |

- Progressive frame = run of 0x80 lines, ended by a run of 0xAB lines. Interlaced (composite):
  `EC×3, AB×18, 80×242 (field 1), AB×2, EC×19, C7×241 (field 2)`. Weave with field 1 (0x80) on even
  lines ([tools/hdcap_fields.py](../tools/hdcap_fields.py)). Component 480i (MST3367 path) uses a
  different field layout: `AB×5, EC×18, C7×240, EC×5, AB×17, 80×240`. A driver must find field
  boundaries from the XY codes rather than assume fixed counts. **Line length and line count follow
  the source's native timing**; the device does not scale. Observed:

| Source | Line payload | Active × blank lines | Total lines | Line rate | Audio frames/line | Rate on USB | Capture |
|---|---|---|---|---|---|---|---|
| VGA 640×480@60 (analog, MST3367 ADC) | 1440 B = **720 px** | 480 × 45 | 525 | ≈31.47 kHz | 1.5253 | ≈45.7 MB/s | c06, c07, c08 |
| HDMI 1280×720@60 (Genesis Mini) | 2560 B = **1280 px** | 720 × 30 | 750 | 45.0 kHz | 1.0665 | ≈64 MB/s | c11 |
| Composite NTSC 480i (NES) | 1440 B = **720 px** | field 1: 242, field 2: 241; blank 42 | 525 (interlaced) | ≈15.73 kHz | 3.0506 | ≈23 MB/s | c12 |
| S-Video NTSC 480i (DVD player) | 1440 B = 720 px | identical to composite | 525 (interlaced) | ≈15.73 kHz | 3.0505 | ≈23 MB/s | c13 |
| Component 480p (DVD, progressive scan) | 1440 B = 720 px | 480 × 45 (same as VGA) | 525 | ≈31.47 kHz | 1.5253 | ≈45.7 MB/s | c14 |
| Component 480i (DVD) | 1440 B = 720 px | 240 + 240 active; blank AB×5, EC×18 / EC×5, AB×17 | 525 (interlaced) | ≈15.73 kHz | 3.05 | ≈23 MB/s | c14 |

- VGA: the 720-sample window contains the 640-pixel picture offset about 62 px right and 24
  lines down. Cropping/positioning is needed (probably MST3367 ADC sampling/position registers;
  TBD). HDMI: the picture fills the window exactly (it follows the source's DE timing).
- Audio records are paced to the line rate so the total is always 48 kHz: N=3/N=2 mix for VGA,
  mostly N=2 with occasional N=3 for 720p, N=3/N=4 mix for NTSC composite.
- Audio: **48,000 Hz**, stereo, 16-bit signed LE (VGA: 1.5253 frames/line × 31,469 lines/s; 720p: 1.0665 × 45,000).
  HDMI embedded audio in c11 is real stereo (L/R correlation 0.91, not identical).
  **Confirmed with real program audio in c08** (−18 dBFS music on the first channel of each frame; the user confirmed by ear that the decoded clip matches the source).
  L/R order is assumed, pending a known left/right test. Audio is
  multiplexed into the same isochronous stream; there is no separate audio endpoint.
- The device sends the source's native timing. The app's
  output size (e.g. 320×240) is scaled on the host and does not change USB traffic.
- Reference demuxer: [tools/hdcap_demux.py](../tools/hdcap_demux.py) (`--linelen`, `--active` per mode) (written from these
  observations, not from vendor code). It renders `analysis/c06/demux/frame_*.png` and `audio.wav`.

## Input switching (observed)
On every input change the driver stops the stream (alt 0), runs the `C1` sequence, and writes about
72 registers: MST3367 0x51, a 0x98 sequence (`20=05, 00=01, 10=FE, 20=75`), a 0x88 decoder block,
and the 0x94 16-register block. It then restarts the stream (alt 1) if a signal is present.
Composite and S-Video bursts differ only in decoder regs 0x02/0x06 (see the table above).
Component (480p and 480i) is digitized by the **MST3367**, not the 0x88 decoder: the driver detected
the 480p → 480i source change by itself (alt 0 → reconfigure → alt 1 within 0.55 s). The burst was
161 MST3367 writes + 2 to 0x98 (`analysis/c14_burst_480p_to_480i.txt`). The Component input-select burst was captured in c15 (`analysis/c15_burst_5_to_component.txt`,
about 280 writes: 252 MST3367, 10 to 0x98, 16 to 0x94, 2 to 0x88).
MST3367 final-state comparison, VGA 640×480 vs Component (`analysis/mst_state_vga640x480.txt`,
`analysis/mst_state_component.txt`). Input and timing differences are mixed together; candidates:
- **bank 0 reg 0x51 = MST3367 input select (confirmed by driver logic + wire):**
  `0x01` analog RGB (VGA), `0x21` analog YPbPr (Component), `0x81` HDMI (`0x89` on some host/board
  combinations), `0x80` during reset. → bit 7 digital/analog, bit 5 YPbPr/RGB.
- **bank 0 regs 0x01/0x02 = ADC PLL divider**: `(r01 << 4) | (r02 >> 4)` = h-total − 1
  (Component 480p: 0x359 = 857 → 858, the CEA-861 value; VGA source: 0x36F → 880). Set per mode.
- bank 0 regs 0x0F (08/20), 0x2C (CC/9D), 0x39 (00/98), 0x81–0x86. Possibly input/colorspace.
- bank 0 regs 0x01–0x0D, 0x92–0xA8. Likely mode/timing (PLL, clamp, window), not input select.
Separating input from timing needs a VGA and a Component capture at the same timing, or the
MST3367 register documentation/decompilation.
Burst listings: `analysis/c13_burst_to_composite.txt`, `analysis/c13_burst_to_svideo.txt`.

## Audio source selection
Not a user command at the USB level. The driver polls **MST3367 bank 2 reg 0x0B** on the HDMI path;
if bits 0x60 are both set (embedded audio present) it uses embedded audio, otherwise line-in, then
applies it through its mode-set routine. This is why the app's embedded/line-in switch produced
no immediate traffic in c05.

## Driver behaviour notes
- The MST3367 mode-detection poll (~150 ms period, bank 0/2 reads, reg 0x0E cycling 0x40/0x48/0x4C)
  **continues after the application closes**.
- Switching the app's audio source (embedded → line in) produced **no I2C or control traffic**
  in c05. Either the change is host-side, or it takes effect at the next stream start.
- During Composite with no source, the stream stayed off (alt 0) and only the decoder at 0x88 was polled.

## Register meanings used by libhdcap (2026-10-18)

TW9900 (public datasheet FN7767 rev 5.00):
- 0x01 STATUS1: bit 7 VDLOSS, bit 6 HLOCK, bit 5 SLOCK, bit 3 VLOCK, bit 1 MONO, bit 0 DET50.
- 0x02 INFORM: bit 6 FC27 (27 MHz), bits 5-4 IFSEL (00 composite, 01 S-Video), bits 3-2 YSEL.
  Captured values: 0x40 composite, 0x54 S-Video.
- 0x06 ACNTL: bit 7 soft reset; bits 3/2/1 clock, luma ADC and chroma ADC power-down.
  0x0E = idle/powered down; 0x42 composite (chroma ADC off); 0x40 S-Video.
- 0x1C SDT: bits 6-4 detected standard; bits 2-0 = 7 means auto-detect.

CS53L21 (public datasheet DS700F2), as written before streaming:
- 0x03=0x4E: mic preamps and bias powered down.
- 0x04=0x44 then 0x46: master mode, I2S, then DIGMIX.
- 0x07=0x00: AIN1 inputs, not muted.
- 0x0A-0x0D=0x00: 0 dB.
- 0x0E/0x0F=0x05: ADC mixer volume.
- 0x18=0x00: channel mixer.
- 0x1C=0xC0: ALC enable bits.
- 0xFF is not a CS53L21 register.

MST3367 measurement registers. Checked against the Windows driver's polling in captures c05 (VGA
480p), c11 (HDMI 720p) and c14/c15 (component 480p/480i), and live on HDMI 720p60, DVI 1080p60 and
VGA 640x480 (2026-10-19). Several meanings were first suggested by Steven Toth's GPL hdcapm driver
for the USB2HDCAPM, which uses the same receiver.

| Bank/reg | Bits | Meaning | Examples |
|---|---|---|---|
| 0 / 0x57, 0x58 | 0x57[5:0]:0x58, **14 bits** | Line period in counts of a ~160.15 MHz reference | 3559 at 45.0 kHz, 5089 at 31.47 kHz, **10180 at 15.73 kHz** (480i: 0x57 = 0x27, which needs bit 13) |
| 0 / 0x59, 0x5A | 14 bits | Field period in reference/1280 counts (~125.1 kHz) | 2085 = 60.00 Hz (720p60), 2087 = 59.94 Hz |
| 0 / 0x5B, 0x5C | 0x5B[2:0]:0x5C | Lines per field | 750, 525, 1125. **Analog interlaced reads 271/272 for 480i**: equalizing and serration pulses count as lines, so use it only for progressive signals |
| 0 / 0x5F | bit 1 | Interlaced | 480i 0x0A/0x0F (bits 0 and 2 alternate per field), progressive 0x00 |
| 0 / 0x6A, 0x6B | 0x6A[3:0]:0x6B | HDMI/DVI horizontal total (pixels) | 1650 (720p60), 2200 (1080p60) |
| 2 / 0x28, 0x29 | 0x29[4:0]:0x28 | HDMI/DVI active width (pixels) | 1280, 1920 |
| 0 / 0x55 | | Status | 0x7F on locked HDMI/DVI, 0x03 on VGA/component |

The driver computes the line rate from 0x57/0x58. The field rate comes from the line count when
progressive (cross-checked against 0x59/0x5A) and from 0x59/0x5A when interlaced. Interlace comes from
0x5F. For HDMI/DVI, the active width sets the expected stream rate exactly (any mode, not only table
modes), and htotal x line rate gives the pixel clock (74.25 / 148.47 MHz measured). `tests/mst_decode_test.c`
replays the captured register values.

Several MST3367/TW9900 details below were cross-checked against **mlaux/usb3hdcap** (Matthew Laux's
GPL-2.0 Linux driver for this card). Each was verified against our captures or the datasheets as
noted, and no code was taken (see the project's research notes for the full comparison).

Further hdcapm claims, not yet checked here: bank 0 0x50 = chip revision 1; 0x55 & 0x3C = TMDS signal
present; 0xB7 bit 1 = hot-plug off; 0xB8 0x10→0x00 HDCP reset; bank 2 0x07 0xF4→0x04 HDMI core reset;
bank 1 0x01 bit 2 HDMI (vs DVI) and bit 0 HDCP active, bank 1 0x34 bit 7 HDCP.

### MST3367 analog front end by pixel clock (2026-10-19)
Bank 0: 0x03 PLL gain, 0x05/0x06 clamp delay/width, 0x07 sync output width, 0x39 clamp level
(component: 0x05 + 0x06 − 0x78). libhdcap chooses them from the mode's pixel clock:

| Pixel clock | 0x03 | 0x05 | 0x06 | 0x07 | Evidence |
|---|---|---|---|---|---|
| < 20 MHz (480i/576i) | 08 | 08 | 08 | 18 | equals the Windows state in captures c14/c15; live component 480i |
| 20–45 MHz (480p, VGA 640x480–800x600) | 20 | 08 | 08 | 18 (VGA: table's 20) | VGA capture c05 (~27 MHz); live component 480p, VGA 640x480 and 800x600 |
| 45–105 MHz (720p, 1080i, VESA) | 58 | 38 | 20 | 30 | Windows driver's component table, via mlaux/usb3hdcap; **live VGA**: 1024x768, 1152x864, 1280x720, 1360x768 (65–85.5 MHz) |
| > 105 MHz (1080p, VESA) | E8 | 38 | 30 | A0 | same; **live VGA**: 1280x1024, 1440x900, 1600x900, 1600x1200 (106.5–162 MHz). Component HD not verified |

VGA clamps differently from component (0x2C = 0xCC, 0x39 = 0x00 in the VGA table), so 0x39 is
only written for component.

VGA verified live on 2026-10-19 (a Windows PC through an active VGA converter): every mode from 640x480 to
1600x1200@60 at 60 fps, sharp and correctly placed. 1600x900@60 (reduced blanking) and 1280x960@60
have identical totals (1800 x 1000, 108 MHz). Bank 0 0x14 does not tell them apart (it does not
track sync polarity), so Auto picks 1600x900 and 1280x960 must be chosen manually. Before this, every mode kept the input table's 480i or 640x480 value.

### MST3367 analog sampling and crop window (verified live, 2026-10-18)
Bank 0:
- 0x01, 0x02[7:4]: ADC PLL divider = h-total − 1 (12 bits).
- 0x80[3:0] + 0x81: window horizontal start (sync + back porch − 1). (Corrected 2026-10-19: an earlier
  reading put bits 11:8 in 0x82[7:4]. That was indistinguishable while starts stayed below 256, and
  shifted VGA 1024x768, start 295, by 256 samples.)
- 0x82[3:0] + 0x83: window width. **The width sets the line length in the USB stream** (720 → 1440 B,
  640 → 1280 B).
- 0x84: window vertical start (per field).
- 0x85[5]: interlaced (field) mode.
- 0x85[3:0] + 0x86: window height (per field).

Captured component 480i (h-start 118 = 62+57−1, v-start 18 = 3+15, height 240 + field flag) matches
the CEA-861 timing exactly. Sega Dreamcast VGA uses 640 active pixels on an 858-sample, 27 MHz line:
same line rate and line count as VESA 640×480@60, so it needs a manual choice.

### Digital inputs: InfoFrame, colour matrix, audio source (verified live, 2026-10-18)
- **DVI-D** = HDMI setup with MST3367 0x51 = 0x89 (second TMDS port) and CPLD 0x01 = 0x01.
- **MST3367 bank 2 0x48–0x4B = AVI InfoFrame PB1–PB4.**
  - PB1 bits 6:5: 00 RGB, 01 YCbCr 4:2:2, 10 YCbCr 4:4:4.
  - PB2 bits 7:6: colorimetry.
  - PB3 bits 3:2: RGB quantization range.
  - PB4: VIC.
  - Latching requires reading **bank 2 0x0B** first (read-to-acknowledge; the first read after a
    reset is stale). Bank 2 0x0B bits 6:5 = 11 means embedded audio present.
- **Colour matrix, bank 0 0x93–0xAA:** nine 15-bit two's-complement coefficients (4096 = 1.0),
  grouped by input R, G, B; within each group the outputs are Cr, Y, Cb. Then offsets Cr, Y, Cb
  (64 = 1 code). YCbCr input uses the R/G/B slots for Cr/Y/Cb, so pass-through is the identity
  matrix with zero offsets. The vendor HDMI table is BT.709 for limited RGB; the VGA table is
  BT.601 for full-range RGB.
- **CPLD 0x20 bit 4:** audio source, 0 = HDMI embedded, 1 = line-in (CS53L21). With embedded audio
  selected but none present, the board floods the stream with silent audio records, which also
  broke video: short lines and an apparent ~180 MB/s ceiling.
- **Stream bandwidth:** every line (blanking included) is sent at the active width.
  SuperSpeed alt 2 (32 KiB/125 µs) carries 1080p60 (259.8 MB/s) on this Mac.

### Composite/S-Video standard detection (verified live, Dreamcast composite)
- The vendor tables set TW9900 SDT 0x1C = 0x07 (auto).
- After a Dreamcast power-up, auto-detection latched **PAL-60 for an NTSC source**: the colour was
  killed (U = V = 128 everywhere); forcing PAL-60 showed a chroma checkerboard. Forcing NTSC-M
  gave correct colour.
- Restarting detection after lock (SDTR 0x1D bit 7 ATSTART) and waiting for SDT bit 7 (DETSTUS)
  to clear gives NTSC-M reliably. libhdcap does this by default and offers a manual override
  (the vendor INF also exposes a standard setting, defaulting to NTSC-M).
- The same initial PAL-60 latch happened with a DVD player on S-Video (2026-10-19); the restart
  corrected it.

### Composite/S-Video picture window, progressive sources, picture controls (2026-10-19)
- **Picture window.** The input tables end with the window the Windows driver sets for NTSC: CROP_HI
  0x07 = 0x02, VDELAY 0x14, VACTIVE 0xF2, HDELAY 0x10, HACTIVE 0x2D0, VVBI 0x55 bit 4 set. For
  625-line standards (PAL-B/D/G/H/I, SECAM, PAL-N) libhdcap writes VACTIVE_HI = 1 into CROP_HI bits
  5:4 (VACTIVE 0x120 = 288 lines per field), VDELAY 0x19, HDELAY 0x0A, and clears VVBI bit 4. The
  values come from mlaux/usb3hdcap, whose NTSC branch matches our captures exactly; the layout
  follows the datasheet. **Not yet verified with a PAL source.**
- **Progressive sources.** STATUS2 0x31 bit 3 (NINTL) = non-interlaced (datasheet). For 240p/288p
  (retro consoles) libhdcap emits each field as a frame with lines doubled (720x480/576), instead of
  weaving fields from different frames. Verified live: a 480i DVD reads 0x31 = 0x90 (interlaced);
  a **Super Nintendo** on composite reads 0x31 = 0x08 (non-interlaced) and gives clean 720x480p at
  60 fps. The decoder still alternates its field window on 240p (239 and 240 active lines,
  top-aligned, content in the same place), so the demuxer pads to a constant 240 (288) lines per
  field.
- **Picture controls** (datasheet): 0x10 brightness (signed), 0x11 contrast (%, 0x64 = 100%), 0x12
  bits 3:0 sharpness (upper bits 0x50 kept), 0x13/0x14 U/V gain (0x80 = 100%), 0x15 hue (signed,
  NTSC only). The Windows defaults, BRIGHT 0xF7, CONTRAST 0x79, SAT 0x80, HUE 0x00, SHARPNESS 0x52, are
  libhdcap's defaults. Verified live on S-Video: saturation 0 gives grey, and brightness raises
  the level.
