import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

const _historyKey = 'ups_status_history_v1';
const _maximumHistoryEntries = 5000;
const _minimumPersistInterval = Duration(seconds: 30);
const _settingsPrefix = 'ups_device_settings_';
const _keepScreenAwakeKey = 'keep_screen_awake_while_monitoring';
const _uartServiceUuid = '6E400001-B5A3-F393-E0A9-E50E24DCCA9E';
const _uartRxUuid = '6E400002-B5A3-F393-E0A9-E50E24DCCA9E';
const _uartTxUuid = '6E400003-B5A3-F393-E0A9-E50E24DCCA9E';
const _maximumSyncedHistorySamples = 100;

void main() {
  runApp(const UpsDashcamMonitorApp());
}

class UpsDashcamMonitorApp extends StatelessWidget {
  const UpsDashcamMonitorApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'UPS Dashcam Monitor',
    theme: ThemeData(colorSchemeSeed: Colors.teal, brightness: Brightness.dark),
    home: const MonitorPage(),
  );
}

class UpsStatus {
  const UpsStatus({
    required this.deviceId,
    required this.receivedAt,
    required this.voltageMv,
    required this.externalSupplyMv,
    required this.switchOn,
    required this.lowBattery,
    required this.bleConnected,
    required this.wifiConnected,
    required this.timeSynchronized,
    required this.rssi,
  });

  final String deviceId;
  final DateTime receivedAt;
  final int voltageMv;
  final int? externalSupplyMv;
  final bool switchOn;
  final bool lowBattery;
  final bool bleConnected;
  final bool wifiConnected;
  // Null is used for advertisements emitted by older firmware that did not
  // expose its retained-clock state.
  final bool? timeSynchronized;
  final int rssi;

  // Firmware payload: <battery mV>;<switch>;<low battery>;<BLE>;<Wi-Fi>;<external mV>;<time synced>
  static UpsStatus? fromAdvertisement({
    required String deviceId,
    required List<int> bytes,
    required int rssi,
    DateTime? receivedAt,
  }) {
    final text = utf8.decode(bytes, allowMalformed: true);
    final match = RegExp(
      r'^(\d+);([01]);([01]);([01]);([01]);(\d+)(?:;([01]))?$',
    ).firstMatch(text);
    final legacyMatch = RegExp(
      r'V=(\d+);S=([01]);L=([01]);B=([01]);W=([01])',
    ).firstMatch(text);
    if (match == null && legacyMatch == null) return null;
    final fields = match ?? legacyMatch!;
    return UpsStatus(
      deviceId: deviceId,
      receivedAt: receivedAt ?? DateTime.now(),
      voltageMv: int.parse(fields.group(1)!),
      externalSupplyMv: match == null ? null : int.parse(match.group(6)!),
      switchOn: fields.group(2) == '1',
      lowBattery: fields.group(3) == '1',
      bleConnected: fields.group(4) == '1',
      wifiConnected: fields.group(5) == '1',
      timeSynchronized: match == null
          ? null
          : (fields.group(7) == null ? null : fields.group(7) == '1'),
      rssi: rssi,
    );
  }

  Map<String, Object?> toJson() => {
    'deviceId': deviceId,
    'receivedAt': receivedAt.toIso8601String(),
    'voltageMv': voltageMv,
    'externalSupplyMv': externalSupplyMv,
    'switchOn': switchOn,
    'lowBattery': lowBattery,
    'bleConnected': bleConnected,
    'wifiConnected': wifiConnected,
    'timeSynchronized': timeSynchronized,
    'rssi': rssi,
  };

  factory UpsStatus.fromJson(Map<String, dynamic> json) => UpsStatus(
    deviceId: json['deviceId'] as String,
    receivedAt: DateTime.parse(json['receivedAt'] as String),
    voltageMv: json['voltageMv'] as int,
    externalSupplyMv: json['externalSupplyMv'] as int?,
    switchOn: json['switchOn'] as bool,
    lowBattery: json['lowBattery'] as bool,
    bleConnected: json['bleConnected'] as bool,
    wifiConnected: json['wifiConnected'] as bool,
    timeSynchronized: json['timeSynchronized'] as bool?,
    rssi: json['rssi'] as int,
  );
}

class HistoryStore {
  Future<List<UpsStatus>> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_historyKey);
    if (raw == null) return [];
    try {
      return (jsonDecode(raw) as List<dynamic>)
          .map((item) => UpsStatus.fromJson(item as Map<String, dynamic>))
          .toList();
    } catch (_) {
      return [];
    }
  }

  Future<void> save(List<UpsStatus> entries) async {
    final prefs = await SharedPreferences.getInstance();
    final retained = entries.length <= _maximumHistoryEntries
        ? entries
        : entries.sublist(entries.length - _maximumHistoryEntries);
    await prefs.setString(
      _historyKey,
      jsonEncode(retained.map((entry) => entry.toJson()).toList()),
    );
  }

  Future<void> clear() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_historyKey);
  }
}

enum LoadOverride { calendar, forceOn, forceOff }

class WeeklyInterval {
  const WeeklyInterval({required this.startMinute, required this.endMinute});
  final int startMinute;
  final int endMinute;

  Map<String, int> toJson() => {'start': startMinute, 'end': endMinute};
  factory WeeklyInterval.fromJson(Map<String, dynamic> json) => WeeklyInterval(
    startMinute: json['start'] as int,
    endMinute: json['end'] as int,
  );
}

class SyncedVoltageSample {
  const SyncedVoltageSample({
    required this.sequence,
    required this.receivedAt,
    required this.voltageMv,
    required this.lowBattery,
    required this.switchOn,
    required this.measurementValid,
  });

  final int sequence;
  // Null means the ESP32 recorded the sample before receiving a valid phone
  // time. Such samples are retained for diagnostics but cannot be charted.
  final DateTime? receivedAt;
  final int voltageMv;
  final bool lowBattery;
  final bool switchOn;
  final bool measurementValid;
}

class _HistoryWireRecord {
  const _HistoryWireRecord({
    required this.sequence,
    required this.epochSeconds,
    required this.voltageMv,
    required this.flags,
  });

  final int sequence;
  final int epochSeconds;
  final int voltageMv;
  final int flags;

  bool get lowBattery => flags & 0x01 != 0;
  bool get switchOn => flags & 0x02 != 0;
  bool get measurementValid => flags & 0x04 == 0;
}

class SyncHistoryStore {
  static const _channel = MethodChannel('ups_dashcam_monitor/foreground_sync');

  Future<void> clear() => _channel.invokeMethod<void>('clearSyncedHistory');

  Future<List<SyncedVoltageSample>> load() async {
    final result = await _channel.invokeMethod<Map<Object?, Object?>>(
      'getSyncedHistory',
    );
    final encoded = result?['data'] as String? ?? '';
    if (encoded.isEmpty) return [];
    try {
      final bytes = base64Decode(encoded);
      if (bytes.length < 4 ||
          bytes[0] != 1 ||
          bytes[1] > _maximumSyncedHistorySamples)
        return [];
      final count = bytes[1];
      if (bytes.length < 4 + count * 11) return [];
      final data = ByteData.sublistView(Uint8List.fromList(bytes));
      final records = List<_HistoryWireRecord>.generate(count, (index) {
        final offset = 4 + index * 11;
        return _HistoryWireRecord(
          sequence: data.getUint32(offset, Endian.little),
          epochSeconds: data.getUint32(offset + 4, Endian.little),
          voltageMv: data.getUint16(offset + 8, Endian.little),
          flags: data.getUint8(offset + 10),
        );
      });
      return List<SyncedVoltageSample>.generate(count, (index) {
        final record = records[index];
        return SyncedVoltageSample(
          sequence: record.sequence,
          receivedAt: record.epochSeconds == 0
              ? null
              : DateTime.fromMillisecondsSinceEpoch(record.epochSeconds * 1000),
          voltageMv: record.voltageMv,
          lowBattery: record.lowBattery,
          switchOn: record.switchOn,
          measurementValid: record.measurementValid,
        );
      });
    } catch (_) {
      return [];
    }
  }
}

class DeviceSettings {
  const DeviceSettings({
    this.veryLowBatteryMv = 3000,
    this.lowBatteryMv = 3300,
    this.recoveryMv = 3600,
    this.externalSupplyHighMv = 11000,
    this.overrideMode = LoadOverride.calendar,
    this.calendarEnabled = false,
    required this.intervals,
  });

  factory DeviceSettings.defaults() => DeviceSettings(
    intervals: List<List<WeeklyInterval>>.generate(7, (_) => const []),
  );

  final int veryLowBatteryMv;
  final int lowBatteryMv;
  final int recoveryMv;
  final int externalSupplyHighMv;
  final LoadOverride overrideMode;
  final bool calendarEnabled;
  final List<List<WeeklyInterval>> intervals;

  DeviceSettings copyWith({LoadOverride? overrideMode}) => DeviceSettings(
    veryLowBatteryMv: veryLowBatteryMv,
    lowBatteryMv: lowBatteryMv,
    recoveryMv: recoveryMv,
    externalSupplyHighMv: externalSupplyHighMv,
    overrideMode: overrideMode ?? this.overrideMode,
    calendarEnabled: calendarEnabled,
    intervals: intervals
        .map((day) => List<WeeklyInterval>.from(day))
        .toList(),
  );

  Map<String, Object> toJson() => {
    'veryLowBatteryMv': veryLowBatteryMv,
    'lowBatteryMv': lowBatteryMv,
    'recoveryMv': recoveryMv,
    'externalSupplyHighMv': externalSupplyHighMv,
    'overrideMode': overrideMode.index,
    'calendarEnabled': calendarEnabled,
    'intervals': intervals
        .map((day) => day.map((interval) => interval.toJson()).toList())
        .toList(),
  };

  factory DeviceSettings.fromJson(Map<String, dynamic> json) {
    final rawDays = json['intervals'] as List<dynamic>? ?? const [];
    final days = List<List<WeeklyInterval>>.generate(7, (day) {
      if (day >= rawDays.length || rawDays[day] is! List<dynamic>)
        return const [];
      return (rawDays[day] as List<dynamic>)
          .whereType<Map<String, dynamic>>()
          .map(WeeklyInterval.fromJson)
          .take(4)
          .toList();
    });
    final mode = (json['overrideMode'] as int? ?? 0).clamp(
      0,
      LoadOverride.values.length - 1,
    );
    return DeviceSettings(
      veryLowBatteryMv: json['veryLowBatteryMv'] as int? ?? 3000,
      lowBatteryMv: json['lowBatteryMv'] as int? ?? 3300,
      recoveryMv: json['recoveryMv'] as int? ?? 3600,
      externalSupplyHighMv: json['externalSupplyHighMv'] as int? ?? 11000,
      overrideMode: LoadOverride.values[mode],
      calendarEnabled: json['calendarEnabled'] as bool? ?? false,
      intervals: days,
    );
  }

  factory DeviceSettings.fromRegister(String value) {
    final fields = value.trim().split(',');
    final isV1 = fields.length == 34 && fields.first == 'C1';
    final isV2 = fields.length == 35 && fields.first == 'C2';
    final isV3 = fields.length == 36 && fields.first == 'C3';
    if (!isV1 && !isV2 && !isV3) {
      throw const FormatException('Invalid ESP32 settings register.');
    }
    final intervalOffset = isV3 ? 8 : (isV2 ? 7 : 6);
    final intervals = List<List<WeeklyInterval>>.generate(7, (_) => []);
    for (var day = 0; day < 7; day++) {
      for (var slot = 0; slot < 4; slot++) {
        final range = fields[intervalOffset + day * 4 + slot].split(':');
        if (range.length != 2)
          throw const FormatException('Invalid calendar interval.');
        final start = int.parse(range[0]);
        final end = int.parse(range[1]);
        if (start >= 0 && end > start)
          intervals[day].add(
            WeeklyInterval(startMinute: start, endMinute: end),
          );
      }
    }
    final mode = int.parse(
      fields[isV3 ? 5 : (isV2 ? 4 : 3)],
    ).clamp(0, LoadOverride.values.length - 1);
    return DeviceSettings(
      veryLowBatteryMv: isV3 ? int.parse(fields[1]) : 3000,
      lowBatteryMv: int.parse(fields[isV3 ? 2 : 1]),
      recoveryMv: int.parse(fields[isV3 ? 3 : 2]),
      externalSupplyHighMv: isV3
          ? int.parse(fields[4])
          : (isV2 ? int.parse(fields[3]) : 11000),
      overrideMode: LoadOverride.values[mode],
      calendarEnabled: fields[isV3 ? 6 : (isV2 ? 5 : 4)] == '1',
      intervals: intervals,
    );
  }
}

class DeviceSettingsStore {
  Future<DeviceSettings> load(String deviceId) async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString('$_settingsPrefix$deviceId');
    if (raw == null) return DeviceSettings.defaults();
    try {
      return DeviceSettings.fromJson(jsonDecode(raw) as Map<String, dynamic>);
    } catch (_) {
      return DeviceSettings.defaults();
    }
  }

  Future<void> save(String deviceId, DeviceSettings settings) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      '$_settingsPrefix$deviceId',
      jsonEncode(settings.toJson()),
    );
  }
}

class BleConfigurator {
  static const _syncChannel = MethodChannel(
    'ups_dashcam_monitor/foreground_sync',
  );

  static List<String> commandsFor(DeviceSettings settings) {
    final now = DateTime.now();
    final commands = <String>[
      'E,0',
      'T,${now.millisecondsSinceEpoch ~/ 1000},${now.timeZoneOffset.inMinutes}',
      'B,${settings.lowBatteryMv},${settings.recoveryMv}',
      'V,${settings.veryLowBatteryMv}',
      'X,${settings.externalSupplyHighMv}',
      'M,${settings.overrideMode.index}',
    ];
    for (var day = 0; day < 7; day++) {
      for (var slot = 0; slot < 4; slot++) {
        final interval = slot < settings.intervals[day].length
            ? settings.intervals[day][slot]
            : null;
        commands.add(
          interval == null
              ? 'I,$day,$slot,-1,-1'
              : 'I,$day,$slot,${interval.startMinute},${interval.endMinute}',
        );
      }
    }
    commands.add('E,${settings.calendarEnabled ? 1 : 0}');
    return commands;
  }

  static Future<void> startForegroundSync(
    String deviceId,
    DeviceSettings settings,
  ) async {
    await _syncChannel.invokeMethod<void>('startForegroundSync', {
      'deviceId': deviceId,
      'commands': commandsFor(settings),
    });
  }

  // This deliberately transfers time only. An automatic first sync must not
  // overwrite configuration that may already have been set on the ESP32.
  static Future<void> startInitialTimeSync(String deviceId) async {
    final now = DateTime.now();
    await _syncChannel.invokeMethod<void>('startForegroundSync', {
      'deviceId': deviceId,
      'commands': [
        'T,${now.millisecondsSinceEpoch ~/ 1000},${now.timeZoneOffset.inMinutes}',
      ],
    });
  }

  static Future<void> startLoadOverride(
    String deviceId,
    LoadOverride overrideMode,
  ) => _syncChannel.invokeMethod<void>('startForegroundSync', {
    'deviceId': deviceId,
    'commands': ['M,${overrideMode.index}'],
  });

  static Future<bool> isForegroundSyncActive() async =>
      await _syncChannel.invokeMethod<bool>('isForegroundSyncActive') ?? false;

  static Future<_ForegroundSyncResult> foregroundSyncResult() async {
    final result = await _syncChannel.invokeMethod<Map<Object?, Object?>>(
      'getForegroundSyncResult',
    );
    return _ForegroundSyncResult(
      message: result?['message'] as String? ?? 'Synchronization ended.',
      success: result?['success'] as bool? ?? false,
      sampleCount: result?['sampleCount'] as int? ?? 0,
    );
  }

  static Future<void> cancelForegroundSync() =>
      _syncChannel.invokeMethod<void>('cancelForegroundSync');
}

class _ForegroundSyncResult {
  const _ForegroundSyncResult({
    required this.message,
    required this.success,
    required this.sampleCount,
  });

  final String message;
  final bool success;
  final int sampleCount;
}

class MonitorPage extends StatefulWidget {
  const MonitorPage({super.key});

  @override
  State<MonitorPage> createState() => _MonitorPageState();
}

class _MonitorPageState extends State<MonitorPage> with WidgetsBindingObserver {
  final _store = HistoryStore();
  final _syncHistoryStore = SyncHistoryStore();
  final _settingsStore = DeviceSettingsStore();
  final List<UpsStatus> _history = [];
  final Map<String, UpsStatus> _latestByDevice = {};
  List<SyncedVoltageSample> _syncedHistory = [];
  StreamSubscription<List<ScanResult>>? _scanSubscription;
  String? _selectedDeviceId;
  String? _error;
  bool _scanning = false;
  bool _keepScreenAwake = false;
  bool _appIsForeground = true;
  final Set<String> _automaticTimeSyncDevices = {};
  bool _loadOverrideSending = false;
  String? _loadOverrideMessage;

  UpsStatus? get _latest =>
      _selectedDeviceId == null ? null : _latestByDevice[_selectedDeviceId];

  List<String> get _deviceIds => {
    ..._history.map((entry) => entry.deviceId),
    ..._latestByDevice.keys,
  }.toList()..sort();

  List<UpsStatus> get _visibleHistory => _selectedDeviceId == null
      ? _history
      : _history.where((entry) => entry.deviceId == _selectedDeviceId).toList();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _loadAndStart();
  }

  Future<void> _loadAndStart() async {
    final results = await Future.wait([
      _store.load(),
      _syncHistoryStore.load(),
    ]);
    final stored = results[0] as List<UpsStatus>;
    final synced = results[1] as List<SyncedVoltageSample>;
    final preferences = await SharedPreferences.getInstance();
    if (!mounted) return;
    setState(() {
      _history.addAll(stored);
      _syncedHistory = synced;
      _keepScreenAwake = preferences.getBool(_keepScreenAwakeKey) ?? false;
      for (final entry in _history) {
        _latestByDevice[entry.deviceId] = entry;
      }
      if (_history.isNotEmpty) _selectedDeviceId = _history.last.deviceId;
    });
    await _startScan();
    unawaited(_applyScreenWakeLock());
  }

  Future<void> _refreshSyncedHistory() async {
    final synced = await _syncHistoryStore.load();
    if (mounted) setState(() => _syncedHistory = synced);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _appIsForeground = state == AppLifecycleState.resumed;
    if (_appIsForeground) {
      unawaited(_refreshSyncedHistory());
    }
    unawaited(_applyScreenWakeLock());
  }

  Future<void> _applyScreenWakeLock() async {
    try {
      if (_keepScreenAwake && _appIsForeground) {
        await WakelockPlus.enable();
      } else {
        await WakelockPlus.disable();
      }
    } catch (error) {
      // Screen wake is optional; a platform-plugin issue must never prevent BLE
      // scanning or monitoring.
      if (mounted) {
        setState(
          () =>
              _error = 'Could not apply the keep-screen-awake setting: $error',
        );
      }
    }
  }

  Future<void> _setKeepScreenAwake(bool enabled) async {
    setState(() => _keepScreenAwake = enabled);
    final preferences = await SharedPreferences.getInstance();
    await preferences.setBool(_keepScreenAwakeKey, enabled);
    await _applyScreenWakeLock();
  }

  Future<void> _startScan() async {
    if (FlutterBluePlus.isScanningNow) {
      if (mounted) setState(() => _scanning = true);
      return;
    }
    try {
      if (!await FlutterBluePlus.isSupported) {
        setState(
          () => _error = 'Bluetooth LE is not supported on this device.',
        );
        return;
      }
      await FlutterBluePlus.adapterState
          .where((state) => state == BluetoothAdapterState.on)
          .first;
      _scanSubscription ??= FlutterBluePlus.onScanResults.listen(
        _handleResults,
        onError: (Object error) =>
            mounted ? setState(() => _error = error.toString()) : null,
      );
      await FlutterBluePlus.startScan(
        continuousUpdates: true,
        removeIfGone: const Duration(seconds: 15),
      );
      if (mounted) setState(() => _scanning = true);
    } catch (error) {
      if (mounted) setState(() => _error = error.toString());
    }
  }

  Future<void> _stopScan() async {
    await FlutterBluePlus.stopScan();
    if (mounted) setState(() => _scanning = false);
  }

  void _handleResults(List<ScanResult> results) {
    for (final result in results) {
      // flutter_blue_plus removes the two-byte manufacturer ID and exposes the
      // remaining data in each map value. Iterate rather than relying on a
      // platform-specific integer representation of 0xFFFF.
      for (final bytes in result.advertisementData.manufacturerData.values) {
        final status = UpsStatus.fromAdvertisement(
          deviceId: result.device.remoteId.str,
          bytes: bytes,
          rssi: result.rssi,
        );
        if (status != null) _accept(status);
      }
    }
  }

  void _accept(UpsStatus received) {
    final historicalForDevice = _history
        .where((entry) => entry.deviceId == received.deviceId)
        .toList();
    final previousForDevice = historicalForDevice.isEmpty
        ? null
        : historicalForDevice.last;
    final stateChanged =
        previousForDevice == null ||
        previousForDevice.switchOn != received.switchOn ||
        previousForDevice.lowBattery != received.lowBattery ||
        previousForDevice.wifiConnected != received.wifiConnected ||
        previousForDevice.bleConnected != received.bleConnected;
    final shouldPersist =
        previousForDevice == null ||
        stateChanged ||
        received.receivedAt.difference(previousForDevice.receivedAt) >=
            _minimumPersistInterval;
    setState(() {
      _latestByDevice[received.deviceId] = received;
      _selectedDeviceId ??= received.deviceId;
      if (shouldPersist) _history.add(received);
    });
    if (shouldPersist) {
      unawaited(_store.save(_history));
    }
    if (received.timeSynchronized == false && _appIsForeground) {
      unawaited(_startAutomaticInitialTimeSync(received.deviceId));
    }
  }

  Future<void> _startAutomaticInitialTimeSync(String deviceId) async {
    if (!_automaticTimeSyncDevices.add(deviceId)) return;
    var stoppedScan = false;
    try {
      if (await BleConfigurator.isForegroundSyncActive()) return;
      await _stopScan();
      stoppedScan = true;
      await BleConfigurator.startInitialTimeSync(deviceId);

      // The foreground service owns scanning until its bounded sync finishes.
      // Wait before querying so Android has time to enter onStartCommand.
      await Future<void>.delayed(const Duration(milliseconds: 500));
      while (mounted && await BleConfigurator.isForegroundSyncActive()) {
        await Future<void>.delayed(const Duration(seconds: 1));
      }
      if (!mounted) return;
      final result = await BleConfigurator.foregroundSyncResult();
      if (!result.success) {
        setState(
          () => _error =
              'Automatic first time sync failed: ${result.message}. Open Device settings and tap Sync to ESP32 to retry.',
        );
      }
    } catch (error) {
      if (mounted) {
        setState(
          () => _error =
              'Could not start automatic first time sync: $error',
        );
      }
    } finally {
      _automaticTimeSyncDevices.remove(deviceId);
      if (mounted && stoppedScan) await _startScan();
    }
  }

  Future<void> _setLoadOverride(LoadOverride overrideMode) async {
    final deviceId = _selectedDeviceId;
    if (deviceId == null || _loadOverrideSending) return;
    if (await BleConfigurator.isForegroundSyncActive()) {
      if (mounted) {
        setState(
          () => _error =
              'Another ESP32 synchronization is already in progress.',
        );
      }
      return;
    }

    var stoppedScan = false;
    setState(() {
      _error = null;
      _loadOverrideSending = true;
      _loadOverrideMessage =
          'Waiting for the ESP32 to wake and apply the load override…';
    });
    try {
      final current = await _settingsStore.load(deviceId);
      await _stopScan();
      stoppedScan = true;
      await BleConfigurator.startLoadOverride(deviceId, overrideMode);

      await Future<void>.delayed(const Duration(milliseconds: 500));
      while (mounted && await BleConfigurator.isForegroundSyncActive()) {
        await Future<void>.delayed(const Duration(seconds: 1));
      }
      if (!mounted) return;
      final result = await BleConfigurator.foregroundSyncResult();
      if (!result.success) {
        setState(
          () => _error = 'Load override failed: ${result.message}',
        );
        return;
      }
      await _settingsStore.save(deviceId, current.copyWith(overrideMode: overrideMode));
      if (mounted) {
        setState(
          () => _loadOverrideMessage =
              'Load is forced ${overrideMode == LoadOverride.forceOn ? 'ON' : 'OFF'}.',
        );
      }
    } catch (error) {
      if (mounted) {
        setState(() => _error = 'Could not apply load override: $error');
      }
    } finally {
      if (mounted) setState(() => _loadOverrideSending = false);
      if (mounted && stoppedScan) await _startScan();
    }
  }

  Future<void> _clearHistory() async {
    await Future.wait([_store.clear(), _syncHistoryStore.clear()]);
    if (mounted) {
      setState(() {
        _history.clear();
        _syncedHistory = [];
        _latestByDevice.clear();
        _selectedDeviceId = null;
      });
    }
  }

  Future<void> _openSettings() async {
    final deviceId = _selectedDeviceId;
    if (deviceId == null) return;
    final current = await _settingsStore.load(deviceId);
    if (!mounted) return;
    final updated = await Navigator.of(context).push<DeviceSettings>(
      MaterialPageRoute(
        builder: (_) =>
            SettingsPage(deviceId: deviceId, initialSettings: current),
      ),
    );
    if (updated != null) await _settingsStore.save(deviceId, updated);
    await _startScan();
  }

  Future<void> _openTerminal() async {
    final deviceId = _selectedDeviceId;
    if (deviceId == null) return;
    await Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => BleTerminalPage(deviceId: deviceId)),
    );
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    unawaited(WakelockPlus.disable());
    _scanSubscription?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final latest = _latest;
    return Scaffold(
      appBar: AppBar(
        title: const Text('UPS Dashcam Monitor'),
        actions: [
          IconButton(
            tooltip: _scanning ? 'Stop scan' : 'Start scan',
            icon: Icon(
              _scanning
                  ? Icons.stop_circle_outlined
                  : Icons.play_circle_outline,
            ),
            onPressed: _scanning ? _stopScan : _startScan,
          ),
          IconButton(
            tooltip: 'Clear history',
            icon: const Icon(Icons.delete_outline),
            onPressed: _clearHistory,
          ),
          IconButton(
            tooltip: 'Refresh synchronized history',
            icon: const Icon(Icons.sync),
            onPressed: _refreshSyncedHistory,
          ),
          IconButton(
            tooltip: 'Device settings',
            icon: const Icon(Icons.tune),
            onPressed: _selectedDeviceId == null ? null : _openSettings,
          ),
          IconButton(
            tooltip: 'BLE terminal',
            icon: const Icon(Icons.terminal),
            onPressed: _selectedDeviceId == null ? null : _openTerminal,
          ),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            if (_error != null) _ErrorBanner(message: _error!),
            if (_deviceIds.length > 1)
              DropdownButtonFormField<String>(
                initialValue: _selectedDeviceId,
                decoration: const InputDecoration(
                  labelText: 'Advertising device',
                ),
                items: _deviceIds
                    .map((id) => DropdownMenuItem(value: id, child: Text(id)))
                    .toList(),
                onChanged: (id) => setState(() => _selectedDeviceId = id),
              ),
            const SizedBox(height: 12),
            _StatusCard(status: latest, scanning: _scanning),
            const SizedBox(height: 12),
            Card(
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'Immediate load control',
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 4),
                    const Text(
                      'Applied during the next ESP32 awake BLE window. Force ON never overrides battery protection.',
                    ),
                    const SizedBox(height: 12),
                    Row(
                      children: [
                        Expanded(
                          child: FilledButton.icon(
                            onPressed: latest == null || _loadOverrideSending
                                ? null
                                : () => _setLoadOverride(LoadOverride.forceOn),
                            icon: const Icon(Icons.power),
                            label: const Text('Force ON'),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: FilledButton.icon(
                            style: FilledButton.styleFrom(
                              backgroundColor: Theme.of(context).colorScheme.error,
                              foregroundColor: Theme.of(context).colorScheme.onError,
                            ),
                            onPressed: latest == null || _loadOverrideSending
                                ? null
                                : () => _setLoadOverride(LoadOverride.forceOff),
                            icon: const Icon(Icons.power_off),
                            label: const Text('Force OFF'),
                          ),
                        ),
                      ],
                    ),
                    if (_loadOverrideMessage != null) ...[
                      const SizedBox(height: 8),
                      Text(_loadOverrideMessage!),
                    ],
                  ],
                ),
              ),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Keep screen awake while monitoring'),
              subtitle: const Text(
                'Prevents display sleep only while this app is in the foreground.',
              ),
              value: _keepScreenAwake,
              onChanged: _setKeepScreenAwake,
            ),
            const SizedBox(height: 20),
            Text(
              'Voltage history',
              style: Theme.of(context).textTheme.titleLarge,
            ),
            const SizedBox(height: 8),
            const _HistoryLegend(),
            const SizedBox(height: 8),
            SizedBox(
              height: 220,
              child: VoltageHistoryChart(
                advertisements: _visibleHistory,
                synchronized: _syncedHistory,
              ),
            ),
            const SizedBox(height: 16),
            Text(
              '${_visibleHistory.length} advertisement samples · ${_syncedHistory.length} synchronized samples',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 8),
            ..._visibleHistory.reversed
                .take(12)
                .map((entry) => _HistoryRow(status: entry)),
          ],
        ),
      ),
    );
  }
}

class BleTerminalPage extends StatefulWidget {
  const BleTerminalPage({super.key, required this.deviceId});
  final String deviceId;

  @override
  State<BleTerminalPage> createState() => _BleTerminalPageState();
}

class _BleTerminalPageState extends State<BleTerminalPage> {
  final _input = TextEditingController();
  final List<String> _lines = [];
  late final BluetoothDevice _device;
  BluetoothCharacteristic? _rx;
  StreamSubscription<List<int>>? _txSubscription;
  StreamSubscription<List<ScanResult>>? _advertisementSubscription;
  String? _error;
  bool _connecting = true;
  bool _waitingForAdvertisement = true;
  bool _connectingAfterAdvertisement = false;
  bool _terminalStartedScan = false;

  @override
  void initState() {
    super.initState();
    _device = BluetoothDevice.fromId(widget.deviceId);
    _waitForAdvertisement();
  }

  void _append(String line) {
    if (!mounted) return;
    setState(() {
      _lines.add(line);
      if (_lines.length > 200) _lines.removeAt(0);
    });
  }

  Future<void> _waitForAdvertisement() async {
    _append('Waiting for the next ESP32 BLE advertisement…');
    try {
      if (!await FlutterBluePlus.isSupported) {
        throw StateError('Bluetooth LE is not supported on this device.');
      }
      await FlutterBluePlus.adapterState
          .where((state) => state == BluetoothAdapterState.on)
          .first;
      _advertisementSubscription ??= FlutterBluePlus.onScanResults.listen(
        _handleAdvertisements,
        onError: (Object error) {
          if (mounted) setState(() => _error = 'BLE scan failed: $error');
        },
      );
      if (!FlutterBluePlus.isScanningNow) {
        await FlutterBluePlus.startScan(
          continuousUpdates: true,
          removeIfGone: const Duration(seconds: 15),
        );
        _terminalStartedScan = true;
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _error = 'Could not wait for the ESP32 advertisement: $error';
          _connecting = false;
          _waitingForAdvertisement = false;
        });
      }
    }
  }

  void _handleAdvertisements(List<ScanResult> results) {
    if (_connectingAfterAdvertisement) return;
    final seen = results.any(
      (result) =>
          result.device.remoteId.str.toUpperCase() ==
          widget.deviceId.toUpperCase(),
    );
    if (seen) unawaited(_connectAfterAdvertisement());
  }

  Future<void> _connectAfterAdvertisement() async {
    if (_connectingAfterAdvertisement) return;
    _connectingAfterAdvertisement = true;
    await _advertisementSubscription?.cancel();
    _advertisementSubscription = null;
    if (_terminalStartedScan) {
      await FlutterBluePlus.stopScan();
      _terminalStartedScan = false;
    }
    if (mounted) setState(() => _waitingForAdvertisement = false);
    try {
      if (_device.isDisconnected)
        await _device.connect(license: License.nonprofit, mtu: 185);
      final services = await _device.discoverServices();
      final service = services
          .where((item) => item.uuid == Guid(_uartServiceUuid))
          .firstOrNull;
      final rx = service?.characteristics
          .where((item) => item.uuid == Guid(_uartRxUuid))
          .firstOrNull;
      final tx = service?.characteristics
          .where((item) => item.uuid == Guid(_uartTxUuid))
          .firstOrNull;
      if (rx == null || tx == null) {
        throw StateError('ESP32 UART characteristics were not found.');
      }
      _txSubscription = tx.lastValueStream.listen((bytes) {
        if (bytes.isNotEmpty)
          _append('< ${utf8.decode(bytes, allowMalformed: true)}');
      });
      await tx.setNotifyValue(true);
      if (mounted) {
        setState(() {
          _rx = rx;
          _connecting = false;
        });
      }
      _append('Connected. Commands: r/reset, s/sleep, m/dump.');
    } catch (error) {
      await _device.disconnect();
      if (mounted) {
        setState(() {
          _error = null;
          _waitingForAdvertisement = true;
        });
      }
      _append(
        'Connection window ended before the terminal connected; waiting for the next advertisement.',
      );
      _connectingAfterAdvertisement = false;
      if (mounted) await _waitForAdvertisement();
      return;
    }
    _connectingAfterAdvertisement = false;
  }

  Future<void> _send() async {
    final command = _input.text.trim();
    if (command.isEmpty || _rx == null) return;
    try {
      await _rx!.write(utf8.encode(command));
      _append('> $command');
      _input.clear();
    } catch (error) {
      _append('! Send failed: $error');
    }
  }

  @override
  void dispose() {
    _input.dispose();
    _txSubscription?.cancel();
    _advertisementSubscription?.cancel();
    if (_terminalStartedScan) unawaited(FlutterBluePlus.stopScan());
    _device.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('ESP32 BLE terminal')),
    body: SafeArea(
      child: Column(
        children: [
          if (_error != null)
            Padding(
              padding: const EdgeInsets.all(8),
              child: _ErrorBanner(message: _error!),
            ),
          if (_connecting) const LinearProgressIndicator(),
          if (_waitingForAdvertisement)
            const Padding(
              padding: EdgeInsets.all(12),
              child: Text('Waiting for the device to wake and advertise…'),
            ),
          Expanded(
            child: ListView.builder(
              padding: const EdgeInsets.all(12),
              itemCount: _lines.length,
              itemBuilder: (_, index) => SelectableText(
                _lines[index],
                style: const TextStyle(fontFamily: 'monospace'),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.all(12),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _input,
                    enabled: _rx != null,
                    decoration: const InputDecoration(labelText: 'Command'),
                    onSubmitted: (_) => _send(),
                  ),
                ),
                IconButton(
                  icon: const Icon(Icons.send),
                  onPressed: _rx == null ? null : _send,
                ),
              ],
            ),
          ),
        ],
      ),
    ),
  );
}

class SettingsPage extends StatefulWidget {
  const SettingsPage({
    super.key,
    required this.deviceId,
    required this.initialSettings,
  });

  final String deviceId;
  final DeviceSettings initialSettings;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  static const _dayNames = [
    'Monday',
    'Tuesday',
    'Wednesday',
    'Thursday',
    'Friday',
    'Saturday',
    'Sunday',
  ];
  late final TextEditingController _lowController;
  late final TextEditingController _veryLowController;
  late final TextEditingController _recoveryController;
  late final TextEditingController _externalSupplyHighController;
  late LoadOverride _overrideMode;
  late bool _calendarEnabled;
  late List<List<WeeklyInterval>> _intervals;
  String? _error;
  bool _sending = false;
  bool _syncActive = false;
  String? _syncMessage;
  Timer? _syncStateTimer;
  final _settingsStore = DeviceSettingsStore();

  bool get _controlsLocked => _sending || _syncActive;

  @override
  void initState() {
    super.initState();
    _lowController = TextEditingController(
      text: widget.initialSettings.lowBatteryMv.toString(),
    );
    _veryLowController = TextEditingController(
      text: widget.initialSettings.veryLowBatteryMv.toString(),
    );
    _recoveryController = TextEditingController(
      text: widget.initialSettings.recoveryMv.toString(),
    );
    _externalSupplyHighController = TextEditingController(
      text: widget.initialSettings.externalSupplyHighMv.toString(),
    );
    _overrideMode = widget.initialSettings.overrideMode;
    _calendarEnabled = widget.initialSettings.calendarEnabled;
    _intervals = widget.initialSettings.intervals
        .map((day) => List<WeeklyInterval>.from(day))
        .toList();
    _refreshSyncState();
  }

  @override
  void dispose() {
    _syncStateTimer?.cancel();
    _lowController.dispose();
    _veryLowController.dispose();
    _recoveryController.dispose();
    _externalSupplyHighController.dispose();
    super.dispose();
  }

  void _startSyncStatePolling() {
    _syncStateTimer?.cancel();
    _syncStateTimer = Timer.periodic(
      const Duration(seconds: 1),
      (_) => _refreshSyncState(),
    );
  }

  Future<void> _refreshSyncState() async {
    final active = await BleConfigurator.isForegroundSyncActive();
    if (!mounted) return;
    if (_syncActive && !active) {
      final result = await BleConfigurator.foregroundSyncResult();
      if (!mounted) return;
      _syncStateTimer?.cancel();
      setState(() {
        _syncActive = false;
        _syncMessage = result.success
            ? result.sampleCount == 0
                  ? '${result.message} 0 history samples synced with this phone.'
                  : 'Synchronization finished. ${result.sampleCount} history ${result.sampleCount == 1 ? 'sample' : 'samples'} synced with this phone. Refresh history on the monitor screen.'
            : 'Synchronization failed: ${result.message}';
      });
    } else if (!_syncActive && active) {
      setState(() => _syncActive = true);
      _startSyncStatePolling();
    }
  }

  String _formatMinute(int minute) {
    final hour = minute ~/ 60;
    final remainder = minute % 60;
    return '${hour.toString().padLeft(2, '0')}:${remainder.toString().padLeft(2, '0')}';
  }

  Future<void> _editTime(int day, int index, bool start) async {
    final existing = _intervals[day][index];
    final value = start ? existing.startMinute : existing.endMinute;
    final chosen = await showTimePicker(
      context: context,
      initialTime: TimeOfDay(hour: value ~/ 60, minute: value % 60),
    );
    if (chosen == null || !mounted) return;
    final replacement = WeeklyInterval(
      startMinute: start
          ? chosen.hour * 60 + chosen.minute
          : existing.startMinute,
      endMinute: start ? existing.endMinute : chosen.hour * 60 + chosen.minute,
    );
    if (replacement.startMinute >= replacement.endMinute) {
      setState(
        () => _error =
            'An interval must end after it starts. Split overnight periods across two days.',
      );
      return;
    }
    setState(() => _intervals[day][index] = replacement);
  }

  void _addInterval(int day) {
    if (_intervals[day].length >= 4) return;
    setState(
      () => _intervals[day].add(
        const WeeklyInterval(startMinute: 8 * 60, endMinute: 18 * 60),
      ),
    );
  }

  Future<void> _send() async {
    final low = int.tryParse(_lowController.text);
    final veryLow = int.tryParse(_veryLowController.text);
    final recovery = int.tryParse(_recoveryController.text);
    final externalSupplyHigh = int.tryParse(_externalSupplyHighController.text);
    if (veryLow == null ||
        low == null ||
        recovery == null ||
        externalSupplyHigh == null ||
        veryLow < 2000 ||
        veryLow > low ||
        low < 2500 ||
        recovery < low ||
        recovery > 5500 ||
        externalSupplyHigh < 1000 ||
        externalSupplyHigh > 20000) {
      setState(
        () => _error =
            'Use valid thresholds: very low at least 2000 mV, low 2500–5000 mV, and recovery at or above low.',
      );
      return;
    }
    final settings = DeviceSettings(
      veryLowBatteryMv: veryLow,
      lowBatteryMv: low,
      recoveryMv: recovery,
      externalSupplyHighMv: externalSupplyHigh,
      overrideMode: _overrideMode,
      calendarEnabled: _calendarEnabled,
      intervals: _intervals
          .map((day) => List<WeeklyInterval>.from(day))
          .toList(),
    );
    setState(() {
      _error = null;
      _sending = true;
    });
    try {
      // The native foreground service owns BLE scanning until this sync ends.
      // Avoid competing scanner sessions from flutter_blue_plus in this process.
      await FlutterBluePlus.stopScan();
      await BleConfigurator.startForegroundSync(widget.deviceId, settings);
      await _settingsStore.save(widget.deviceId, settings);
      if (mounted) {
        setState(() {
          _syncActive = true;
          _syncMessage =
              'Synchronization is waiting for the ESP32 to advertise. You can cancel it here.';
        });
        _startSyncStatePolling();
      }
    } catch (error) {
      await FlutterBluePlus.startScan(
        continuousUpdates: true,
        removeIfGone: const Duration(seconds: 15),
      );
      if (mounted) {
        final message =
            error is PlatformException &&
                error.code == 'notification_permission_required'
            ? 'Allow notifications, then tap Sync to ESP32 again.'
            : 'Could not start synchronization: $error';
        setState(() => _error = message);
      }
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  Future<void> _cancelSync() async {
    setState(() => _sending = true);
    try {
      await BleConfigurator.cancelForegroundSync();
      if (mounted) {
        _syncStateTimer?.cancel();
        setState(() {
          _syncActive = false;
          _syncMessage = 'Synchronization cancelled.';
        });
      }
    } catch (error) {
      if (mounted)
        setState(() => _error = 'Could not cancel synchronization: $error');
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Device settings')),
    body: SafeArea(
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text(
            'Device ${widget.deviceId}',
            style: Theme.of(context).textTheme.labelLarge,
          ),
          const SizedBox(height: 8),
          const Text(
            'Save starts a temporary Android foreground job. It waits for the ESP32 active BLE window, transfers these settings and time, then stops itself.',
          ),
          const SizedBox(height: 20),
          DropdownButtonFormField<LoadOverride>(
            initialValue: _overrideMode,
            decoration: const InputDecoration(labelText: 'Load control'),
            items: const [
              DropdownMenuItem(
                value: LoadOverride.calendar,
                child: Text('Follow weekly calendar'),
              ),
              DropdownMenuItem(
                value: LoadOverride.forceOn,
                child: Text('Force ON (overrides calendar)'),
              ),
              DropdownMenuItem(
                value: LoadOverride.forceOff,
                child: Text('Force OFF (overrides calendar)'),
              ),
            ],
            onChanged: _controlsLocked
                ? null
                : (value) => setState(() => _overrideMode = value!),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _veryLowController,
            enabled: !_controlsLocked,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'Very-low battery threshold',
              helperText:
                  'Uses the most conservative awake/sleep timing below this voltage.',
              suffixText: 'mV',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _lowController,
            enabled: !_controlsLocked,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'Battery-off threshold',
              suffixText: 'mV',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _recoveryController,
            enabled: !_controlsLocked,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'Battery-on threshold',
              suffixText: 'mV',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _externalSupplyHighController,
            enabled: !_controlsLocked,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'External-supply high trigger',
              helperText:
                  'Uses external awake/sleep timing above this voltage.',
              suffixText: 'mV',
            ),
          ),
          const SizedBox(height: 20),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Enable weekly calendar'),
            subtitle: const Text(
              'Disabled is the default: the calendar allows the load to remain on.',
            ),
            value: _calendarEnabled,
            onChanged: _controlsLocked
                ? null
                : (value) => setState(() => _calendarEnabled = value),
          ),
          if (_calendarEnabled) ...[
            const Text(
              'Each interval is an ON interval. With the calendar enabled, no interval means OFF. Maximum four intervals per day.',
            ),
            const SizedBox(height: 8),
            for (var day = 0; day < 7; day++)
              Card(
                child: ExpansionTile(
                  title: Text(_dayNames[day]),
                  subtitle: Text(
                    _intervals[day].isEmpty
                        ? 'Off all day'
                        : '${_intervals[day].length} on interval(s)',
                  ),
                  children: [
                    for (var index = 0; index < _intervals[day].length; index++)
                      ListTile(
                        title: Text(
                          '${_formatMinute(_intervals[day][index].startMinute)} – ${_formatMinute(_intervals[day][index].endMinute)}',
                        ),
                        onTap: _controlsLocked
                            ? null
                            : () => _editTime(day, index, true),
                        trailing: Wrap(
                          spacing: 0,
                          children: [
                            IconButton(
                              tooltip: 'Edit end',
                              icon: const Icon(Icons.schedule),
                              onPressed: _controlsLocked
                                  ? null
                                  : () => _editTime(day, index, false),
                            ),
                            IconButton(
                              tooltip: 'Remove interval',
                              icon: const Icon(Icons.delete_outline),
                              onPressed: _controlsLocked
                                  ? null
                                  : () => setState(
                                      () => _intervals[day].removeAt(index),
                                    ),
                            ),
                          ],
                        ),
                      ),
                    if (_intervals[day].length < 4)
                      TextButton.icon(
                        onPressed: _controlsLocked
                            ? null
                            : () => _addInterval(day),
                        icon: const Icon(Icons.add),
                        label: const Text('Add on interval'),
                      ),
                  ],
                ),
              ),
          ],
          if (_syncMessage != null)
            Card(
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Text(_syncMessage!),
              ),
            ),
          if (_error != null) _ErrorBanner(message: _error!),
          const SizedBox(height: 16),
          FilledButton.icon(
            onPressed: _sending ? null : (_syncActive ? _cancelSync : _send),
            icon: _sending
                ? const SizedBox(
                    width: 18,
                    height: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(
                    _syncActive
                        ? Icons.cancel_outlined
                        : Icons.bluetooth_connected,
                  ),
            label: Text(
              _sending
                  ? 'Starting sync…'
                  : (_syncActive ? 'Cancel synchronization' : 'Sync to ESP32'),
            ),
          ),
        ],
      ),
    ),
  );
}

class _ErrorBanner extends StatelessWidget {
  const _ErrorBanner({required this.message});
  final String message;

  @override
  Widget build(BuildContext context) => Card(
    color: Theme.of(context).colorScheme.errorContainer,
    child: Padding(
      padding: const EdgeInsets.all(12),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(
            Icons.error_outline,
            color: Theme.of(context).colorScheme.onErrorContainer,
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              message,
              style: TextStyle(
                color: Theme.of(context).colorScheme.onErrorContainer,
              ),
            ),
          ),
        ],
      ),
    ),
  );
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({required this.status, required this.scanning});
  final UpsStatus? status;
  final bool scanning;

  @override
  Widget build(BuildContext context) {
    if (status == null) {
      return Card(
        child: Padding(
          padding: const EdgeInsets.all(20),
          child: Text(
            scanning
                ? 'Scanning for status advertisements…'
                : 'Scan is stopped.',
          ),
        ),
      );
    }
    final age = DateTime.now().difference(status!.receivedAt);
    return Card(
      color: status!.lowBattery
          ? Theme.of(context).colorScheme.errorContainer
          : null,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '${(status!.voltageMv / 1000).toStringAsFixed(3)} V',
              style: Theme.of(context).textTheme.displaySmall,
            ),
            if (status!.externalSupplyMv != null) ...[
              const SizedBox(height: 4),
              Text(
                'External supply ${(status!.externalSupplyMv! / 1000).toStringAsFixed(3)} V',
                style: Theme.of(context).textTheme.titleMedium,
              ),
            ],
            const SizedBox(height: 12),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                _StateChip(
                  label: status!.switchOn ? 'Switch ON' : 'Switch OFF',
                  active: status!.switchOn,
                ),
                _StateChip(
                  label: status!.lowBattery ? 'Battery LOW' : 'Battery OK',
                  active: !status!.lowBattery,
                ),
                _StateChip(
                  label: status!.wifiConnected
                      ? 'Wi-Fi connected'
                      : 'Wi-Fi off',
                  active: status!.wifiConnected,
                ),
                _StateChip(
                  label: status!.bleConnected
                      ? 'BLE connected'
                      : 'BLE advertising',
                  active: status!.bleConnected,
                ),
                if (status!.timeSynchronized != null)
                  _StateChip(
                    label: status!.timeSynchronized!
                        ? 'Time synchronized'
                        : 'Time sync needed',
                    active: status!.timeSynchronized!,
                  ),
              ],
            ),
            const SizedBox(height: 12),
            Text('Last received ${_formatAge(age)} · RSSI ${status!.rssi} dBm'),
          ],
        ),
      ),
    );
  }
}

class _StateChip extends StatelessWidget {
  const _StateChip({required this.label, required this.active});
  final String label;
  final bool active;
  @override
  Widget build(BuildContext context) => Chip(
    label: Text(label),
    backgroundColor: active
        ? Theme.of(context).colorScheme.primaryContainer
        : null,
  );
}

class _HistoryRow extends StatelessWidget {
  const _HistoryRow({required this.status});
  final UpsStatus status;
  @override
  Widget build(BuildContext context) => ListTile(
    dense: true,
    contentPadding: EdgeInsets.zero,
    title: Text(
      '${(status.voltageMv / 1000).toStringAsFixed(3)} V · ${status.lowBattery ? 'LOW' : 'OK'}',
    ),
    subtitle: Text(
      '${status.receivedAt.toLocal()} · switch ${status.switchOn ? 'ON' : 'OFF'}',
    ),
    trailing: Text('${status.rssi} dBm'),
  );
}

class _HistoryLegend extends StatelessWidget {
  const _HistoryLegend();

  @override
  Widget build(BuildContext context) => Wrap(
    spacing: 16,
    runSpacing: 4,
    children: [
      _LegendItem(color: Colors.red, label: 'Live advertisements'),
      _LegendItem(color: Colors.blue, label: 'ESP32 synchronized history'),
    ],
  );
}

class _LegendItem extends StatelessWidget {
  const _LegendItem({required this.color, required this.label});
  final Color color;
  final String label;

  @override
  Widget build(BuildContext context) => Row(
    mainAxisSize: MainAxisSize.min,
    children: [
      Container(
        width: 10,
        height: 10,
        decoration: BoxDecoration(color: color, shape: BoxShape.circle),
      ),
      const SizedBox(width: 6),
      Text(label, style: Theme.of(context).textTheme.bodySmall),
    ],
  );
}

class VoltageHistoryChart extends StatelessWidget {
  const VoltageHistoryChart({
    super.key,
    required this.advertisements,
    required this.synchronized,
  });
  final List<UpsStatus> advertisements;
  final List<SyncedVoltageSample> synchronized;

  @override
  Widget build(BuildContext context) {
    final pointCount =
        advertisements.length +
        synchronized.where((sample) => sample.measurementValid).length;
    return CustomPaint(
      painter: _VoltageChartPainter(
        advertisements,
        synchronized,
        Theme.of(context).colorScheme,
      ),
      child: pointCount < 2
          ? const Center(
              child: Text(
                'Receive or synchronize at least two samples to draw history.',
              ),
            )
          : null,
    );
  }
}

class _ChartPoint {
  const _ChartPoint(this.time, this.voltageMv);
  final DateTime time;
  final int voltageMv;
}

class _VoltageChartPainter extends CustomPainter {
  const _VoltageChartPainter(
    this.advertisements,
    this.synchronized,
    this.colors,
  );
  final List<UpsStatus> advertisements;
  final List<SyncedVoltageSample> synchronized;
  final ColorScheme colors;

  List<_ChartPoint> _advertisementPoints() =>
      advertisements
          .where((sample) => sample.voltageMv > 0)
          .map((sample) => _ChartPoint(sample.receivedAt, sample.voltageMv))
          .toList()
        ..sort((a, b) => a.time.compareTo(b.time));

  List<_ChartPoint> _synchronizedPoints() =>
      synchronized
          .where(
            (sample) =>
                sample.measurementValid &&
                sample.voltageMv > 0 &&
                sample.receivedAt != null,
          )
          .map((sample) => _ChartPoint(sample.receivedAt!, sample.voltageMv))
          .toList()
        ..sort((a, b) => a.time.compareTo(b.time));

  @override
  void paint(Canvas canvas, Size size) {
    final advertisementPoints = _advertisementPoints();
    final synchronizedPoints = _synchronizedPoints();
    final allPoints = [...advertisementPoints, ...synchronizedPoints];
    if (allPoints.length < 2) return;
    const left = 42.0;
    const bottom = 24.0;
    final values = allPoints
        .map((entry) => entry.voltageMv.toDouble())
        .toList();
    final minValue = (values.reduce(math.min) - 100).floorToDouble();
    final maxValue = (values.reduce(math.max) + 100).ceilToDouble();
    final range = math.max(1.0, maxValue - minValue);
    final graph = Rect.fromLTWH(
      left,
      8,
      size.width - left - 8,
      size.height - bottom - 8,
    );
    final minTime = allPoints
        .map((point) => point.time.millisecondsSinceEpoch)
        .reduce(math.min);
    final maxTime = allPoints
        .map((point) => point.time.millisecondsSinceEpoch)
        .reduce(math.max);
    final timeRange = math.max(1, maxTime - minTime);
    final axis = Paint()..color = colors.outline;
    canvas.drawLine(
      Offset(graph.left, graph.top),
      Offset(graph.left, graph.bottom),
      axis,
    );
    canvas.drawLine(
      Offset(graph.left, graph.bottom),
      Offset(graph.right, graph.bottom),
      axis,
    );

    void drawSeries(List<_ChartPoint> points, Color color) {
      if (points.isEmpty) return;
      final path = Path();
      for (var i = 0; i < points.length; i++) {
        final point = points[i];
        final x =
            graph.left +
            graph.width *
                (point.time.millisecondsSinceEpoch - minTime) /
                timeRange;
        final y =
            graph.bottom - graph.height * (point.voltageMv - minValue) / range;
        if (i == 0) {
          path.moveTo(x, y);
        } else {
          path.lineTo(x, y);
        }
        canvas.drawCircle(Offset(x, y), 2.5, Paint()..color = color);
      }
      if (points.length > 1) {
        canvas.drawPath(
          path,
          Paint()
            ..color = color
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2,
        );
      }
    }

    drawSeries(advertisementPoints, Colors.red);
    drawSeries(synchronizedPoints, Colors.blue);
    final labelPainter = TextPainter(textDirection: TextDirection.ltr);
    for (final value in [minValue, maxValue]) {
      labelPainter.text = TextSpan(
        text: '${(value / 1000).toStringAsFixed(2)} V',
        style: TextStyle(color: colors.onSurface, fontSize: 11),
      );
      labelPainter.layout();
      final y = graph.bottom - graph.height * (value - minValue) / range;
      labelPainter.paint(canvas, Offset(0, y - 7));
    }
  }

  @override
  bool shouldRepaint(covariant _VoltageChartPainter oldDelegate) =>
      oldDelegate.advertisements != advertisements ||
      oldDelegate.synchronized != synchronized ||
      oldDelegate.colors != colors;
}

String _formatAge(Duration age) {
  if (age.inSeconds < 5) return 'just now';
  if (age.inMinutes < 1) return '${age.inSeconds}s ago';
  if (age.inHours < 1) return '${age.inMinutes}m ago';
  return '${age.inHours}h ago';
}
