# UPS Dashcam Monitor

Flutter mobile application that receives the ESP32's connectionless BLE status
advertisements. It displays the latest status immediately and stores a bounded,
local voltage/status history on the phone.

## Wire protocol

The firmware broadcasts BLE manufacturer data with manufacturer identifier
`0xFFFF` and this ASCII payload:

```text
V=<mV>;S=<0|1>;L=<0|1>;B=<0|1>;W=<0|1>
```

`S`, `L`, `B`, and `W` correspond to the `Status`, `LowBattery`, `BLE`, and
`WiFi` values returned by the ESP32 `dump` command. The app accepts any
advertisement matching this payload; it does not need to connect to the ESP32.

## Build

Flutter is not installed in this repository's development environment. Install
the Flutter SDK, then from this directory create the platform runners and run:

```bash
flutter create --platforms=android,ios .
flutter pub get
flutter run
```

Before building, add the following Android declarations to
`android/app/src/main/AndroidManifest.xml`, immediately below the `<manifest>`
element:

```xml
<uses-feature android:name="android.hardware.bluetooth_le" android:required="false" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />
```

Set Android `minSdk` to 21 or higher. Add this key to the iOS runner's
`Info.plist`:

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Bluetooth is used to receive UPS status advertisements.</string>
```

The app saves one history point every 30 seconds per device, plus immediate
points for any switch, low-battery, BLE, or Wi-Fi state change. History is
limited to 5,000 points and can be cleared from the app bar.
