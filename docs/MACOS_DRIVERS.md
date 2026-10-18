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
  host time of the last write). Clients read behind the write position; the HAL plug-in reports
  its zero timestamps from the daemon's clock anchor.
- **Control:** XPC messages to change input/mode/standard/offsets and to query status (signal,
  resolution, frame rate).

## Signing and distribution
| Component | Local development | Distribution |
|---|---|---|
| hdcapd, test client | ad-hoc | Developer ID + notarization |
| HAL plug-in (`/Library/Audio/Plug-Ins/HAL`) | ad-hoc should load in coreaudiod (needs sudo to install, restart coreaudiod) | Developer ID + notarization |
| Camera Extension + host app | needs a provisioning profile with the System Extension capability (paid Developer Program), or SIP disabled + `systemextensionsctl developer on` | Developer ID + notarization |

## Status
See the README's status section; this document describes the target design.
