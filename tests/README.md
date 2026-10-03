# Keymap tests

Each directory is a ZMK keymap test: `native_sim.keymap` presses keys on a mock
matrix (by `row, col`, see `../boards/shields/kinesisble/kinesisble-transform.dtsi`),
`events.patterns` filters the log, and `keycode_events.snapshot` is the expected
output. The tests include the real keymaps, so they catch keymap regressions.

Useful matrix positions:

| Key | RC |
|---|---|
| Q | (1,1) |
| Caps/Esc (`&mt CLCK ESC`) | (2,0) |
| Left Shift/`(` | (3,0) |
| J | (2,7) |
| M | (3,7) |
| `=` | (0,0) |
| W | (1,2) |
| Left thumb top-left (`&mo 1` / Ctrl) | (6,5) |
| Program | (5,13) |

Run them all from a ZMK checkout (Linux x86-64, e.g. the ZMK dev container):

    ZMK_SRC_DIR=zmk/app zmk/app/run-test.sh tests

Set `ZMK_TESTS_AUTO_ACCEPT=1` to rewrite snapshots after an intended change,
then review the diff before committing.
