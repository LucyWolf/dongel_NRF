# Headpat Dongle

USB-Dongle firmware for the Headpat haptic headpat system. Plugged into the PC — receives motor commands from the Headpat Server via USB serial and forwards them over BLE to the worn Headpat device.

## How it works

```
Headpat Server  →  USB Serial  →  Dongle  →  BLE  →  Headpat
```

- Listens on USB CDC serial for motor commands from the Headpat Server
- Scans for and connects to a paired Headpat device over BLE
- Forwards motor nibble data and control commands transparently
- Stores up to 5 paired device addresses in flash (LittleFS)

## Supported Hardware

| Board | UF2 file |
|---|---|
| Pro Micro nRF52840 (nice!nano, etc.) | `dongle-pro-micro-nrf52840.uf2` |
| Holyiot nRF52840 USB Dongle | `dongle-holyiot-nrf52840.uf2` |

Both boards use the Adafruit nRF52 bootloader. Download the latest firmware from [Releases](../../releases).

## Pairing

1. Open the Headpat Server and click **Pair**
2. On the Headpat device, hold the button for 3 seconds to enter pairing mode (fast LED blink)
3. The Dongle scans for the ecosystem UUID and saves the address to flash
4. On next startup the Dongle connects automatically

## Flashing

1. Double-tap the reset button — the board mounts as a USB drive (`NRF52BOOT`)
2. Copy the correct `.uf2` file onto the drive
3. The device reboots automatically

The [Headpat Server](https://github.com/LucyWolf/Headpat-Server) can detect the drive and flash automatically when an update is available.

## Building from source

Requires [PlatformIO](https://platformio.org/).

```bash
# both boards
pio run

# specific board
pio run -e nicenano
pio run -e holyiot
```

Builds automatically on every push via GitHub Actions.

## Serial commands

| Command | Description |
|---|---|
| `info` | Show firmware version, connection status, uptime |
| `list` | List saved (paired) devices |
| `pairing` | Enter pairing mode (60 s) |
| `remove` | Remove currently connected device |
| `clear` | Remove all saved devices |
| `dfu` | Enter UF2 bootloader |
| `reboot` | Reboot |
| `uptime` | Show uptime |

## Related

- [Headpat](https://github.com/LucyWolf/Headpat) — Headpat device firmware
- [Headpat Server](https://github.com/LucyWolf/Headpat-Server) — Windows/Linux app (OSC receiver + BLE bridge)
