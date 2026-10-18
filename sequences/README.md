# Sequence files

Plain-text lists of USB operations, replayed by `hdcap_run_sequence()` / `hdcap-capture -s`.
They are a bootstrap: each one records bus transactions observed between the Windows driver and the
device (with `tools/pcap2seq.py`). The goal is to replace them with logic written from the chip
datasheets and `docs/PROTOCOL.md`.

| Op | Meaning |
|---|---|
| `i2c_w ADDR REG VAL` | I2C register write (vendor request 0xC0 OUT; ADDR is the 8-bit address) |
| `i2c_r ADDR REG` | I2C register read (0xC0 IN); the observed value may follow as a comment |
| `vreq REQ VALUE INDEX` | Vendor OUT request without data (0xC1 = GPIO, 0xC2/0xC5/0xC6/0xC7 init/probes). A stall is tolerated |
| `alt IFACE ALT` | SET_INTERFACE |
| `delay MS` | Pause |

`#` starts a comment.

| File | Source | Content |
|---|---|---|
| `open.seq` | c05, 00:11:17.80–00:11:20.30 | Device open: C2/C7/C5/C6, GPIO-45 reset pulse, chip init, VGA input configuration |
| `select_hdmi.seq` | c05, 00:11:45.10–00:11:46.20 | Switch to HDMI (MST3367 0x51=0x81, CPLD 0x01=0x02). Recorded **without an HDMI signal**, so it lacks the post-lock setup. Superseded by `open_hdmi.seq` |
| `open_hdmi.seq` | c17, 03:46:53.70–03:47:13.90 | **Complete plug-in → HDMI-locked startup** (Genesis Mini 720p): open, HDMI select, post-lock MST3367 setup incl. colour-space matrix. Ends just before the stream starts. **Verified live on macOS: 1280×720p60 + audio** |
| `probe_mst3367.seq` | hand-written | Reads MST3367/CPLD status registers |
| `exp_cpld_pulse.seq` | hand-written experiment | Pulses CPLD 0x20 bit 0 (did not start the stream) |
