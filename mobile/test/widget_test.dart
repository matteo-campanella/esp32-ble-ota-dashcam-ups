import 'package:flutter_test/flutter_test.dart';
import 'package:ups_dashcam_monitor/main.dart';

void main() {
  test('decodes the ESP32 manufacturer-data status payload', () {
    final status = UpsStatus.fromAdvertisement(
      deviceId: 'AA:BB:CC:DD:EE:FF',
      bytes: '3420;1;0;0;0;13840'.codeUnits,
      rssi: -62,
      receivedAt: DateTime.utc(2026, 9, 20),
    );

    expect(status, isNotNull);
    expect(status!.voltageMv, 3420);
    expect(status.switchOn, isTrue);
    expect(status.lowBattery, isFalse);
    expect(status.externalSupplyMv, 13840);
    expect(status.deviceId, 'AA:BB:CC:DD:EE:FF');
  });
}
