# macOS system-wide drivers: architecture

Goal: make the USB3HDCAP usable by every macOS app, not only OBS. Video goes through a
**CoreMediaIO Camera Extension** (FaceTime, Zoom, QuickTime, browsers, OBS's own camera
source); audio goes through a **Core Audio HAL plug-in** (any app's audio input).

## The constraint
The card delivers video **and audio in one isochronous stream** (audio records are interleaved with
video lines, see PROTOCOL.md). Only one process can claim the USB interface. So neither the camera
extension nor the audio plug-in can own the device; a third process must.

## Components

```
                 +----------------------- hdcapd (LaunchDaemon) -----------------------+
 USB3HDCAP  ---> | libhdcap: hdcap_session (open, select input, detect signal, set up,   |
 (iso stream)    |   stream, demux)                                                     |
                 |   video frames -> IOSurface pool ('yuvs' = packed YUYV, no conversion)|
                 |   audio        -> shared-memory ring buffer (48 kHz s16 stereo)       |
                 |   XPC Mach service: org.open-usb3hdcap.hdcapd                         |
                 +-----------+-----------------------------+-----------------------------+
                             | XPC: frames (IOSurface)     | XPC: ring buffer (xpc_shmem)
                             v                             v
          Camera Extension (CMIOExtension)        HAL plug-in (AudioServerPlugIn, in coreaudiod)
          "USB3HDCAP" camera for all apps          "USB3HDCAP" audio input for all apps
                             ^
          Host app "USB3HDCAP.app": installs the extension (OSSystemExtensionRequest),
          settings UI (input, analog timing, TV standard) -> XPC control messages
```

- **hdcapd** must live in the *system* bootstrap domain (a LaunchDaemon): the camera extension
  runs as `_cmiodalassistants` and the HAL plug-in inside `coreaudiod` (`_coreaudiod`), and both can
  only look up global Mach services. For development the same binary runs as a per-user
  LaunchAgent, loaded transiently with `launchctl bootstrap gui/$UID <plist>`.
- **Video:** one XPC message per frame with an IOSurface (`IOSurfaceCreateXPCObject`), plus
  width/height/timestamp/interlaced. The surfaces come from a small round-robin pool, so clients
  must copy the frame (or finish with it) before the pool wraps. The camera extension copies into
  its own `CVPixelBufferPool`.
- **Audio:** a mapped ring (`xpc_shmem`) with a header (format, capacity, 64-bit write position,
  host time of the last write). Clients read behind the write position. The HAL plug-in reads
  ~100 ms behind the writer and runs its zero timestamps at the **card's** audio rate: the
  card/host ratio comes from the ring's write position against its timestamps, plus a small
  proportional term that holds the read latency on target. Measured on HDMI from a Genesis Mini,
  the card runs about −161 ppm against the host. With a host-paced clock that meant a 100 ms
  replay every ~10 minutes; with tracking, the latency stays flat. Core Audio sees a device
  whose clock is not the host's (`ClockIsStable` 0) and resamples as needed, as it does for
  real USB devices. The device reports `kAudioDevicePropertyLatency` = 4800 frames (the read
  offset), so apps that align by timestamp line audio up with the camera's frames, which carry
  their capture time. `hdcapd-client drift [S]` measures the ratio; `hal-harness` reports
  latency, clock and resync counts.
- **Timestamps:** `hdcap_now_ns()` is `CLOCK_UPTIME_RAW` on macOS (mach_absolute_time in ns), the
  host clock CoreMediaIO and Core Audio use, so frame `time_ns` can be passed straight through.
- **Control:** XPC messages to change input/mode/standard/offsets and to query status (signal,
  resolution, frame rate).

## Signing and distribution
| Component | Local development | Distribution |
|---|---|---|
| hdcapd, test client | ad-hoc | Developer ID + notarization |
| HAL plug-in (`/Library/Audio/Plug-Ins/HAL`) | ad-hoc should load in coreaudiod (needs sudo to install, restart coreaudiod) | Developer ID + notarization |
| Camera Extension + host app | needs a provisioning profile with the System Extension capability (paid Developer Program), or SIP disabled + `systemextensionsctl developer on` | Developer ID + notarization |

## Source layout (`macos/`)
| Path | What |
|---|---|
| `hdcapd/hdcapd.c` | the daemon |
| `hdcapd/hdcapd-client.c` | test client: `status`, `watch [S] [out.ppm]`, `set INPUT …` |
| `common/hdcapd_protocol.h` | XPC protocol and shared ring layout |
| `hal/hdcap_hal.c`, `hal/Info.plist` | HAL plug-in → `build/USB3HDCAP.driver` (universal) |
| `hal/hal-harness.c` | loads the `.driver` in-process like coreaudiod and runs IO against hdcapd |
| `camera/HDCapFrameSource.[hm]` | hdcapd video client: IOSurface → letterboxed NV12 `CVPixelBuffer` (SD 720-wide sources shown 4:3, BT.601 704-sample aperture), black filler without signal |
| `camera/CameraExtension.[hm]`, `camera/main.m` | CMIOExtension provider/device/stream (1080p/720p/480p × 60/30, decimated to the format's rate) |
| `camera/camera-harness.m` | drives the extension classes in-process (no activation): checks frame size, rate, timestamps, format switching, stop/restart |
| `camera/frame-source-test.m` | runs the camera pipeline outside the extension, saves a frame |
| `app/HostApp.m` | `USB3HDCAP.app`: activates the extension, input selector, live status |
| `scripts/dev-agent.sh` | per-user hdcapd for development (`load`/`unload`/`log`) |
| `hdcapd-client drift [S]` | measures the card audio clock against the host clock |
| `scripts/install-system.sh` | `sudo … install|uninstall`: hdcapd LaunchDaemon + HAL plug-in |

Build everything with `make -C macos` (needs `make` at the top level for `libhdcap.a`).

## Status (October 2026)
| Piece | State |
|---|---|
| hdcapd | **verified**: 1280×720 @ 60 fps, 48 kHz audio ring, no gaps, input switching, idle release |
| Camera pipeline (`HDCapFrameSource`) | **verified** via `frame-source-test`: 60 fps, ~1 ms capture→output, correct colour |
| HAL plug-in | **working**: installed system-wide, recorded in QuickTime; card-rate clock tracking verified over 20 minutes |
| Camera extension + host app | **working**: Developer ID signed, notarized, activated; QuickTime records from it (2026-10-19). Also verified in-process via `camera-harness` |

### Signed builds (Developer ID)
Working since 2026-10-19 with the Islandia Logic Ltd. team.
1. In the developer portal, register two App IDs: `<BUNDLE_ID>` with the **System Extension**
   capability, and `<BUNDLE_ID>.camera`. Create a **Developer ID** provisioning profile for each.
   Create **Developer ID Application** and **Developer ID Installer** certificates, and store notary
   credentials with `xcrun notarytool store-credentials <profile> --apple-id … --team-id …`.
2. Copy `macos/signing.mk.example` to `macos/signing.mk` (git-ignored) and fill it in. The camera
   extension is `<BUNDLE_ID>.camera`, the HAL plug-in `<BUNDLE_ID>.hal` and the daemon
   `<BUNDLE_ID>.hdcapd`; the app group and the CMIO Mach service carry the team prefix. The XPC
   service name `org.open-usb3hdcap.hdcapd` stays the same in every build.
3. `make -C macos` signs everything with hardened runtime and secure timestamps. hdcapd is universal
   and links libusb statically (`packaging/build-libusb-universal.sh`).
   `make -C macos notarize-app` notarizes and staples the host app.
4. `make -C macos pkg` builds `build/USB3HDCAP-<version>.pkg`: the app in /Applications, hdcapd as
   a LaunchDaemon (`/Library/Application Support/open-usb3hdcap`, with `uninstall.sh`) and the HAL
   plug-in. It is signed with the Installer identity, notarized and stapled, and the postinstall
   script starts hdcapd and restarts coreaudiod. Then run USB3HDCAP.app, click **Install Camera**
   and approve it in System Settings → General → Login Items & Extensions.
   (Development alternative: `sudo macos/scripts/install-system.sh install` and copy the app by hand.)

Verified: Gatekeeper reports "Notarized Developer ID"; `systemextensionsctl` lists
`com.islandialogic.usb3hdcap.camera` as activated and enabled; QuickTime records from the
"USB3HDCAP" camera (a VHS deck on Composite).

Without an account, the extension can still be tried on a test Mac with SIP disabled and
`systemextensionsctl developer on`.
