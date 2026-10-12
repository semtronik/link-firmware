# link-lr

Firmware for the **Link-LR** handheld: a LoRa P2P field tool on the Link
base board. It receives and sends frames, records every one to flash, and
shows the session's figures.

## Hardware

- **MCU** — ESP32-S3-WROOM-1-N16R8 (dual core 240 MHz, 8 MB PSRAM, 16 MB flash)
- **Radio** — SX1262 over SPI3, on the LR interface module
- **Display** — ST7789T 2.8" 320×240 landscape, SPI2
- **Touch** — FT5426 capacitive, core I2C
- **RTC** — PCF8563 with a backup cell, core I2C
- **Fuel gauge** — MAX17048, core I2C
- **Charger** — BQ24074, status on `~CHG` (GPIO40, open collector)
- **Host link** — native USB Serial/JTAG

The schematic is the reference for every pin. `board_pins.h` mirrors it;
neither invents anything.

## Software

- **Framework** — ESP-IDF v6.1
- **Language** — C
- **UI** — LVGL 9.5
- **JSON** — cJSON
- **Storage** — NVS for settings and profiles, FAT for the event log and
  the recordings

## Build

From an exported ESP-IDF v6.1 shell, in `apps/link-lr/`:

```
idf.py build
idf.py -p <port> flash monitor
```

If `export.ps1` fails to run — it does when the system Python is newer
than the one the IDF virtualenv was built against — source the profile
the installer left instead, and call `idf.py` through the IDF's own
interpreter:

```powershell
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
```

The profile sets `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` and
`ESP_ROM_ELF_DIR` together, which is why sourcing it beats composing the
variables by hand. `tools/release.ps1` works all of this out on its
own.

### First flash

The partition table is custom, so a board that has never carried this
firmware needs its flash cleared first — otherwise the old table stays
and the OTA slots are not where the image expects them:

```powershell
idf.py -p COM<n> erase-flash flash monitor
```

Later flashes are just `flash monitor`.

## Versioning

Releases follow semantic versioning:

- **Major** — a break: a new protocol version, recordings the previous
  Link Studio cannot read, or a partition table change that needs a USB
  flash instead of OTA.
- **Minor** — new features or UI changes, nothing broken.
- **Patch** — fixes only.

Pre-releases carry a suffix: `1.0.0-rc1`.

`PROJECT_VER_BASE` in `CMakeLists.txt` is the version being worked
towards. It is bumped right after a release, so development builds
never carry the number of one already published.

Without an environment variable the build is `1.0.0-dev`, and the splash
carries a build timestamp so two bench builds can be told apart. The
timestamp is regenerated on every `idf.py build`; the version itself is
fixed at CMake configure time, because it is baked into `esp_app_desc`
and is what OTA and `get_version` report.

| Variable | Version | Splash |
|---|---|---|
| none | `1.0.0-dev` | version + build stamp |
| `LINK_RELEASE` | `1.0.0` | version alone |
| `LINK_VERSION=x` | `x` | version alone |

Both are read at CMake configure time, so a tree that last configured
for a release keeps that version until something forces a reconfigure.
After running `release.ps1`, delete `build/` before going back to
ordinary development, or the next build still calls itself a release.

## Releases

From the repository root, naming the app:

```powershell
.\tools\release.ps1 link-lr 1.0.0
```

The script refuses to run on uncommitted changes, on a version that is
neither `PROJECT_VER_BASE` nor a pre-release of it, or when the tag
already exists.

It then sets `LINK_VERSION`, does a full clean build, packages the
result under `apps/link-lr/releases/v1.0.0/`, and tags the commit
`link-lr/v1.0.0`. The tag is local: push it, then bump
`PROJECT_VER_BASE`.

```
releases/v1.0.0/
  link-lr-v1.0.0.bin
  MANIFEST.txt
```

`MANIFEST.txt` carries the build date, the image size and its SHA-256 —
the hash the app checks an OTA image against before it is made
bootable.

The bootloader and partition table are not packaged: boards are flashed
whole once, and updates replace the application image alone.

### Flashing a release

Over USB, on a board that already has its bootloader and partition
table:

```powershell
esptool --chip esp32s3 --port COM<n> write-flash 0x10000 releases\v1.0.0\link-lr-v1.0.0.bin
```

Or over OTA from Link Studio, which pushes the same binary.

Note that rollback is not enabled: an image that flashes but does not
boot has to be replaced over USB.

## Layout

```
.
├── CMakeLists.txt          version resolution, build stamp, LVGL stack
├── partitions.csv          NVS, two OTA slots, FAT
├── sdkconfig.defaults
└── main/
    ├── main.c                boot sequence
    ├── link_radio.c          the radio service: one task owns the SX1262
    ├── link_profile.c        profiles: every radio setting, stored
    ├── link_regul.c          what the regulations say about a transmission
    ├── link_rec.c            the recording: every frame, kept on flash
    ├── link_stats.c          the session's counts, fed by the recording
    ├── link_scan.c           energy sweep over the band
    ├── link_proto_radio.c    the radio JSON commands
    ├── link_proto_rec.c      the recording JSON commands
    ├── ui_manager.c          screen stack and refresh timer
    ├── ui_screen_setup.c     first start: language, region, clock
    ├── ui_screen_capture.c   the main screen: log, stats, action bar
    ├── ui_view_stats.c       the stats page
    ├── ui_screen_config.c    the profile screen
    ├── ui_param_edit.c       one editor, adapting to the parameter
    ├── ui_screen_menu.c      the menu
    ├── ui_screen_scan.c      the band view (not reachable in 1.0)
    ├── ui_sysinfo_radio.c    radio rows on the shared system screen
    └── ui_text.c             the screens' texts, French and English
```

Everything else comes from `../../components/`: the platform screens,
the UI grammar, the clock, the event log, OTA, and the platform half of
the protocol. The screen manager stays here on purpose — each app owns
its navigation.

## Notes

The LoRa sync word is settable, from the Radio screen or with `lr set sync`.
It defaults to `0x12`, the private-network value point-to-point LoRa uses.
Changing it is what makes traffic from a network on another sync word
visible at all — frames are then received and shown as they are, with no
attempt to decode whatever envelope they carry.

`ui_touch.h`, in link-core-ui, holds the three touch thresholds
together, with static assertions on their ordering: only that ordering
makes swipes work over a scrollable list, and it is not obvious from any
one of the values. Read the comment there before changing any of them.
