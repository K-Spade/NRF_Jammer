# NRF24 JAMMER (Flipper Zero FAP)

Native Flipper Zero FAP that controls the serial firmware found in
`cypher-jammer-serial.ino`. Application id: `nrf24_jammer`, menu name:
**NRF24 JAMMER**.

## Safety and scope

Use only with equipment you own or are explicitly authorized to test, inside
an RF-isolated laboratory. The application always sends `stop` when it exits.

## Power

The FAP enables the Flipper Zero's **5 V on GPIO** (OTG) rail as soon as it
starts, so the ESP32 and both nRF24 PA/LNA modules are supplied without any
manual step. There is no menu entry or setting for it: it is simply on by
default. The first UART command waits 3 s after power-up so the ESP32 has time
to finish booting. The rail is switched off again when the app exits.

Power note: two nRF24L01+PA+LNA modules plus an ESP32-WROOM can briefly exceed
what the Flipper's 5 V rail can deliver (especially at `pa max`). If the board
browns out or the radios report `NOT FOUND`, feed the jammer from its own
supply and share only GND with the Flipper.

## Wiring

The application uses the Flipper Zero USART at **115200 baud, 8 data bits, no
parity, 1 stop bit**.

| Flipper Zero | ESP32 |
|---|---|
| Pin 13 TX | UART RX |
| Pin 14 RX | UART TX |
| GND | GND |

TX and RX must be crossed. Both sides must use 3.3 V logic. Do not power the
ESP32 or the nRF24 power-amplifier modules from the Flipper Zero 3.3 V pin.
Use their own correctly sized supply and connect only the common ground and
UART signals.

## User interface

- `Status`, `Help`, `START`, `STOP`, and a five-pass scan are available from
  the main menu.
- `Presets` sends BLE, Bluetooth Classic, Wi-Fi, drone, or full-band presets.
- `Radio settings` controls mode, PA level, bitrate, radio selection, fixed
  channel, scan range, and optional start duration.
- `Device settings` controls autostart, quiet mode, echo, save, reset, and
  reboot.
- `Raw command` exposes the complete line-oriented protocol without requiring
  a new FAP build.

The serial log view has been removed from the UI, so the ESP32 replies are not
shown on the Flipper.

## Supported serial commands

```text
help / ?
status / info
start [seconds]
stop
mode <random|sweep|list|fixed>
channel <0..125>
channels <c1,c2,...>
range <lo> <hi>
preset <ble|bt|wifi|drone|all>
pa <min|low|high|max>
rate <1m|2m|250k>
radios <both|hp|sp>
scan [passes]
autostart <on|off>
quiet <on|off>
echo <on|off>
save
reset
reboot
```

Each command is sent as ASCII followed by `\n`.

## Build with uFBT

Install or update uFBT:

```powershell
py -m pip install --upgrade ufbt
```

Build from this directory:

```powershell
ufbt
```

The resulting application is written below `dist` by uFBT. The FAP ABI must
match the Flipper firmware installed on the device. Rebuild against the target
firmware SDK when using a different release or a custom firmware.
