
# [Wireless Kinesis Advantage BLE](https://github.com/mikewudev/KinesisBLE)

Turns the Kinesis Advantage Keyboard keyboard into a wireless BLE keyboard with a few extra features. Built with Adafruit's nRF52 Feather Arduino boards.

For instructions, see the **[wiki](https://github.com/sysdevmike/KinesisBLE/wiki)**. For more details on the build check out the **[project](https://hackaday.io/project/161578-wireless-ble-kinesis-advantage-custom-controller)**.

This firmware was used with the hardware fork from:
https://github.com/ergodone/KinesisBLE


## Building

From the `app` directory:

    west build -p -b adafruit_feather_nrf52840/nrf52840/uf2 -- -DSHIELD=kinesisble

Enter the bootloader (`&bootloader` on the adjustment layer, or double-tap the
Feather's reset button), then copy `build/zephyr/zmk.uf2` onto the
`FTHR840BOOT` drive. Bluetooth pairings survive reflashing.

### Debug build with USB logging

The normal firmware has no USB serial console, to save power. For a debug
build that logs over USB, add the `zmk-usb-logging` snippet:

    west build -p -b adafruit_feather_nrf52840/nrf52840/uf2 -S zmk-usb-logging -- -DSHIELD=kinesisble

Then read the logs on macOS with `cu -l /dev/tty.usbmodem*` (or `screen`).
The build prints three "assigned n but got y" Kconfig warnings; they are
expected, since the snippet overrides the shield's console defaults.

## Indicator LEDs

- Battery level is shown at power-on (and so on wake, since waking from deep
  sleep restarts the board) and when F24 is pressed: one LED per 25%, or five
  blinks of the caps LED at 10% or below.
- Switching Bluetooth profile or turning on a layer briefly lights LED N for
  profile/layer N (all four for 5 and up).


To restore original Adafruit firmware download from:
https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases
then install with Jlink with following command:
nrfjprog --program feather_nrf52840_express_bootloader-0.4.1_s140_6.1.1.hex --chiperase -f nrf52 --reset