# ZMK config: Kinesis Advantage 2 BLE

ZMK firmware for a Kinesis Advantage 2 converted to Bluetooth with an Adafruit
Feather nRF52840 and an MCP23S17 column expander (the
[KinesisBLE](https://github.com/ergodone/KinesisBLE) hardware).

This repo is a ZMK module: it carries the `kinesisble` shield and keymaps, and
builds against a pinned upstream ZMK, so there is no ZMK fork to rebase.

## Layout

| Path | What |
|---|---|
| `boards/shields/kinesisble/` | The shield: wiring, LED code, config, and the stock Advantage 2 keymap |
| `config/kinesisble.keymap` | My personal keymap (used by default) |
| `config/west.yml` | Pins the ZMK version |
| `build.yaml` | Which firmware variants CI builds |

## Getting firmware

Every push builds three UF2 files in GitHub Actions. Download them from the run's
**Artifacts**:

- `kinesisble.uf2`: personal layout
- `kinesisble-default.uf2`: stock Advantage 2 layout
- `kinesisble-debug.uf2`: personal layout with USB logging (debugging only)

## Flashing

Enter the bootloader (on my layout: hold **Program**, release, then press **F10**;
or double-tap the Feather's reset button), then copy the `.uf2` onto the
`FTHR840BOOT` drive. Bluetooth pairings survive reflashing.

## Updating ZMK

Change the `revision` in `config/west.yml` and the `@<ref>` in
`.github/workflows/build.yml` to the same new ZMK commit or release tag, push,
and check the build.

## Building locally

With a ZMK checkout set up for Zephyr 4.1 (for example via ZMK's dev container),
from its `app` directory:

    west build -p -b adafruit_feather_nrf52840/nrf52840/uf2 -- \
      -DSHIELD=kinesisble \
      -DZMK_EXTRA_MODULES=/path/to/zmk-config-kinesisble \
      -DZMK_CONFIG=/path/to/zmk-config-kinesisble/config

See the [shield readme](boards/shields/kinesisble/Readme.md) for LED meanings and
the debug build.
