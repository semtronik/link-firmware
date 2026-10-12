# link-firmware

ESP32-S3 firmware monorepo. One repo, multiple binaries.

## Structure

- `components/` — shared libraries. Nothing here is flashable.
  - `link-core-board/` — pin map, core I2C, RTC
  - `link-core-power/` — fuel gauge, charge state, power latch
  - `link-core-app/` — clock, event log, preferences, USB, protocol. No LVGL.
  - `link-core-ui/` — display/touch drivers, UI grammar. Not screens.
  - `link-core-screens/` — the screens every app shares
  - `link-mod-<name>/` — one per interface module: driver and test commands
- `apps/<name>/` — one ESP-IDF project per binary
  - `link-lr/` — LoRa P2P product

## Placement

Code used identically by more than one app → `components/`.
Code specific to one app → `apps/<name>/main/`.
When in doubt, keep it in the app. Never copy-paste between apps.

Exception: each app owns its `ui_manager.c` and screen enum, so navigation
can differ. Duplicating that file is expected; duplicating the grammar
under it is not.

## Build

From `apps/<name>/`: `idf.py build` / `idf.py -p <port> flash`.
Never create a project at the repo root.

## Hardware

The schematic is the reference. Never invent a GPIO — if a signal is
missing from `board_pins.h`, stop and ask.

`board_pins.h` selects a revision from `CONFIG_LINK_BOARD_REV_*`. The
`board_pins_rev_*.h` files are generated from the schematic; do not
hand-edit them.

## Comments

Minimal. Only a non-obvious trap warrants one.
