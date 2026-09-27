# UPS Dashcam Monitor

Flutter mobile application that receives the ESP32's connectionless BLE status
advertisements. It displays the latest status immediately and stores a bounded,
local voltage/status history on the phone.

## Wire protocol

The firmware broadcasts BLE manufacturer data with manufacturer identifier
`0xFFFF` and this ASCII payload:

```text
<battery mV>;<switch 0|1>;<low battery 0|1>;<BLE 0|1>;<Wi-Fi 0|1>;<external supply mV>
```

The fields correspond to the `V`, `Status`, `LowBattery`, `BLE`, `WiFi`, and
`ExternalSupply` values returned by the ESP32 `dump` command. The app accepts any
advertisement matching this payload; it does not need to connect to the ESP32.

## Device configuration

Tap the sliders icon after the device appears in the app. Saving starts a
temporary Android foreground service with a visible notification. It scans for
the selected ESP32 in the background, connects during its next active BLE
window, saves the settings below in non-volatile storage, then disconnects and
stops. It times out after 15 minutes if the device is not found, avoiding an
open-ended battery drain. The app UI can therefore be backgrounded or closed
while synchronization is pending.

- battery-off and battery-recovery thresholds;
- a persistent immediate override: follow calendar, force on, or force off;
- an optional weekly calendar with up to four **on** intervals per weekday.

Calendar control is disabled by default, which means the load is allowed on at
all times. Force on/off overrides the calendar but never overrides battery
protection. The phone sends its current time and UTC offset on every save; the
ESP32 retains that time estimate through timer deep sleep. After a complete
power loss, calendar evaluation safely falls back to always-on until the next
phone synchronization. Overnight intervals should be split across two days.

The firmware accepts these compact commands over either the UART RX or Settings
characteristic:

```text
T,<UTC epoch seconds>,<UTC offset minutes>
M,<0 calendar | 1 force on | 2 force off>
B,<low mV>,<recovery mV>
E,<0 calendar disabled | 1 calendar enabled>
I,<weekday Monday=0>,<slot 0-3>,<start minute>,<end minute>
```

An unused interval is sent as `I,<day>,<slot>,-1,-1`. A connection extends the
ESP32's active window to ten seconds, even for a low-battery cycle, allowing a
complete calendar transfer.

The UPS service's Settings characteristic (`235fefc9-58fd-4f84-977a-9a72ae348007`)
is the authoritative configuration register. It is readable and returns a
complete `C1,...` snapshot of the thresholds, override, calendar flag, UTC
offset, and all 28 interval slots. It also accepts the compact configuration
commands above. Background synchronization writes to it.

Each synchronization also reads the service's read-only retained-history
characteristics (`e6aa2d53-4ed4-43a6-a799-18dbf6a6d3da` and
`8e64f238-2ffc-4870-bd31-3358f3b5c82d`). The ESP32 keeps the
latest 100 measurement-cycle records in RTC memory; this survives deep sleep
but is deliberately lost on a complete power loss. The records are read as two
50-sample BLE pages to remain below the ESP32 characteristic-size limit. The voltage graph overlays
these synchronized records in blue, while live BLE advertisement samples are
red. Tap the sync icon in the app bar to refresh
the displayed retained history after a background synchronization completes.

The terminal icon opens a connected BLE UART terminal. It preserves the
firmware's original serial commands: `r` / `reset`, `s` / `sleep`, and `m` /
`dump`.

## Build

From this directory, run:

```bash
flutter pub get
flutter run
```

The Android runner and its Bluetooth permissions are included. For iOS, add
this key to the runner's `Info.plist`:

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Bluetooth is used to receive UPS status advertisements.</string>
```

The app saves one history point every 30 seconds per device, plus immediate
points for any switch, low-battery, BLE, or Wi-Fi state change. History is
limited to 5,000 points and can be cleared from the app bar.
