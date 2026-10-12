# link-firmware

Firmware for the Semtronik **Link** devices: an ESP32-S3 base board with a
touch screen and a battery, and an interface module that makes it a
product.

Part of the Semtronik Link project — product, hardware and protocol:
[github.com/semtronik/link](https://github.com/semtronik/link).

## Products

| App | Device | |
|---|---|---|
| [`apps/link-lr`](apps/link-lr/) | **Link-LR** | LoRa P2P field tool: receives and sends frames, logs every one to flash, shows the session's figures |

Link Studio, the desktop app, drives the device over USB and reads its
recordings back.

## Layout

```
apps/<name>/      one ESP-IDF project per binary
components/       shared code, nothing flashable on its own
  link-core-board/    pin map, core I2C, RTC
  link-core-power/    fuel gauge, charge state, power latch
  link-core-app/      clock, event log, preferences, USB, protocol
  link-core-ui/       display and touch drivers, UI grammar, fonts
  link-core-screens/  the screens every app shares
  link-mod-lr/        the LoRa interface module
tools/            recording download and analysis, screenshots, releases
```

## Build

ESP-IDF **v6.1**. From the app's directory:

```
cd apps/link-lr
idf.py build
idf.py -p <port> flash monitor
```

A board that has never carried this firmware needs its flash erased
first; see [`apps/link-lr/README.md`](apps/link-lr/README.md), which also
covers versioning and releases.

## Flashing a release

Each release attaches the application image and its SHA-256. On a board
that already has its bootloader and partition table:

```
esptool --chip esp32s3 --port <port> write-flash 0x10000 link-lr-v<version>.bin
```

Or from Link Studio, over USB.

## Protocol

The USB protocol and the recording format are specified in the `link`
repository: `protocol/v1/protocol.md` and `protocol/v1/recording.md`.

## Licence

The firmware is released under the **GNU General Public License v3.0**
— see [LICENSE](LICENSE). The hardware is under CERN-OHL-S v2, in
`link-hardware`.

Third-party parts:

- **LVGL** — MIT, fetched by the ESP-IDF component manager.
- **cJSON** — MIT, fetched by the ESP-IDF component manager.
- **Montserrat** — SIL Open Font License 1.1, converted into
  `components/link-core-ui/src/fonts/link_font_*.c`.
- **Font Awesome Free** — glyphs under SIL Open Font License 1.1, merged
  into the same font files for LVGL's symbols.
- **Material Icons** — Apache License 2.0, in
  `components/link-core-ui/src/fonts/link_icons_20.c`.
