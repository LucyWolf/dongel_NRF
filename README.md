# HeatPett Dongle

USB-Dongle firmware for the HeatPett haptic headpat system. Runs on a nice!nano plugged into the PC — receives motor commands from the HeatPett Server via USB serial and forwards them over BLE to the worn HeatPett device.

## How it works

```
HeatPett Server  →  USB Serial  →  Dongle  →  BLE  →  HeatPett
```

- Listens on USB CDC serial for motor commands from the HeatPett Server
- Scans for and connects to a paired HeatPett device over BLE
- Forwards motor nibble data and control commands transparently
- Stores up to 5 paired device addresses in flash (LittleFS)

## Hardware

- nice!nano (nRF52840) plugged into USB

## Pairing

1. Open the HeatPett Server and click **Pair**
2. On the HeatPett device, hold the button for 3 seconds to enter pairing mode (fast LED blink)
3. The Dongle scans for the ecosystem UUID and saves the address to flash
4. On next startup the Dongle connects automatically

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run
```

Builds automatically on every push via GitHub Actions. Download the latest `dongle-firmware.uf2` from [Releases](../../releases).

## Flashing

1. Double-tap the reset button on the nice!nano — it mounts as a USB drive
2. Copy `dongle-firmware.uf2` onto the drive
3. The device reboots automatically

## Related

- [Headpat](https://github.com/LucyWolf/Headpat) — HeatPett device firmware
- HeatPett Server — Windows app (OSC receiver + BLE bridge)
