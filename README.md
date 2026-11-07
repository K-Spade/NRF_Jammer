# NRF24 JAMMER (Flipper Zero FAP)
Native Flipper Zero FAP application to control the companion serial firmware `cypher-jammer-serial.ino`.
- App ID: `nrf24_jammer`
- Menu display name: **NRF24 JAMMER**

> ⚠️ **Legal & Safety Warning**
> This tool is intended exclusively for authorized RF testing. Unauthorized transmission on restricted radio frequencies violates local radio communication regulations.
> Only operate on hardware you own or for which you hold explicit written authorization, inside a shielded RF-isolated laboratory.
> The FAP automatically sends the `stop` command on exit to terminate RF transmission.

## Power Behaviour
The FAP enables the Flipper Zero **5 V GPIO OTG power rail immediately on startup**, supplying power to the ESP32 and dual nRF24 PA/LNA modules automatically; no manual power toggle menu is provided.
- A 3-second delay is inserted before the first UART command to allow the ESP32 to complete its boot sequence.
- The 5 V OTG rail is switched off automatically when exiting the application.

> ⚠️ Power Limitation Note
> Two nRF24L01+PA+LNA modules together with an ESP32-WROOM can draw peak current exceeding the Flipper Zero’s 5 V OTG capacity, especially at `pa max` transmit power.
> If you encounter brownout, `NOT FOUND` radio errors or unstable behaviour: power the jammer hardware from a separate external supply, and connect only common GND and UART signals to the Flipper.
> Do NOT power the ESP32 or nRF24 PA/LNA modules from the Flipper’s 3.3 V pin.

## Wiring & Serial Parameters
UART: **115200 baud, 8N1 (8 data bits, no parity, 1 stop bit)**
Logic level: 3.3 V on both sides.

| Flipper Zero | ESP32 Jammer Board |
|---|---|
| Pin 13 TX | UART RX |
| Pin 14 RX | UART TX |
| GND | GND |

> TX and RX must be crossed. Do not feed 5 V signals into this UART link.

## User Interface Overview
Main menu items:
- `Status`: View current runtime state
- `Help`: Quick reference
- `START` / `STOP`: Manual transmission control
- `5-pass Scan`: Fast band scan routine
- `Presets`: One-click profiles for BLE, Bluetooth Classic, Wi‑Fi, Drone, or Full‑Band operation
- `Radio settings`: Mode, PA power level, bitrate, radio selection, fixed channel, scan range, timed transmit duration
- `Device settings`: Autostart, quiet mode, serial echo, save configuration, reset, ESP32 reboot
- `Raw command`: Direct serial console for sending any supported protocol command without rebuilding the FAP

> Serial log view is removed from the UI. ESP32 responses are **not displayed on the Flipper screen**.

## Serial Command Reference
All commands are ASCII text, terminated with newline `\n`
