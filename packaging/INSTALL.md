# USB3HDCAP plugin for OBS Studio (macOS)

An open-source OBS source for the **StarTech USB3HDCAP** USB 3.0 capture card
(USB ID `1164:F533`, a YUAN UB-530 design). No vendor driver is needed on macOS.

## Requirements
- macOS 13 or later, on Apple Silicon or Intel (the plugin is a universal binary).
- OBS Studio 30 or later.
- **A USB 3 connection: port, hub and cable.** HDMI, DVI and VGA video do not fit through
  USB 2.0. If the card is connected at USB 2.0 speed, the plugin logs a warning
  (OBS: Help > Log Files). A USB 2.0-only cable is a common cause.

## Install
1. Quit OBS.
2. Unzip the download and run, in Terminal:

   ```
   cd ~/Downloads/usb3hdcap-obs-*-macos-universal
   ./install.sh
   ```

   This copies `usb3hdcap.plugin` to `~/Library/Application Support/obs-studio/plugins/`.
   The plugin is not notarized, so the script also removes the download quarantine flag;
   otherwise macOS would block it from loading.

   To install by hand, copy `usb3hdcap.plugin` to that folder, then run
   `/usr/bin/xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/usb3hdcap.plugin`.
3. Start OBS. In **Sources**, click **+** and choose **USB3HDCAP Capture**.

## Settings
| Setting | Meaning |
|---|---|
| **Input** | HDMI, DVI (digital), VGA, Component, Composite, S-Video |
| **Analog timing** | VGA/Component only. **Auto** matches the measured signal against standard VESA/CEA modes. Choose a mode manually if the picture is stretched. Sega Dreamcast VGA: pick *640x480@60 (27 MHz…)* |
| **TV standard** | Composite/S-Video only. **Auto** detects it; force NTSC-M/PAL etc. if colours are missing |
| **Horizontal / Vertical position** | Fine-tune the analog capture window |

HDMI/DVI colour format and the audio source (embedded HDMI audio or the card's line-in) are
detected automatically.

## Supported and tested
Tested on an Intel Mac (macOS 15) with OBS 32: HDMI 720p60, DVI up to 1920×1080p60, VGA
640×480, and Composite NTSC, with audio. Apple Silicon, S-Video, Component and high-resolution
analog VGA are implemented but have not been tested yet; reports are welcome.

## Troubleshooting
- **No picture:** check OBS: Help > Log Files > View Current Log for lines starting with
  `[usb3hdcap]`. They show the detected signal, colour format, audio source and stream rate.
- **"open: LIBUSB_ERROR_ACCESS":** another program is using the card. Only one OBS source
  (or other app) can use it at a time.
- **Card not found:** check that it appears in System Information > USB as *USB3HDCAP*.

## Uninstall
Quit OBS and delete `~/Library/Application Support/obs-studio/plugins/usb3hdcap.plugin`.

## Licences
open-usb3hdcap is free software under the **GNU General Public License v2.0 or later** (`LICENSE`).
Source code: the open-usb3hdcap repository. The plugin bundles libusb (LGPL-2.1-or-later). Both
licence texts are also in `usb3hdcap.plugin/Contents/Resources/licenses/`.
