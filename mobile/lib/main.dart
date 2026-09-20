import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

const _historyKey = 'ups_status_history_v1';
const _maximumHistoryEntries = 5000;
const _minimumPersistInterval = Duration(seconds: 30);

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
    required this.switchOn,
    required this.lowBattery,
    required this.bleConnected,
    required this.wifiConnected,
    required this.rssi,
  });

  final String deviceId;
  final DateTime receivedAt;
  final int voltageMv;
  final bool switchOn;
  final bool lowBattery;
  final bool bleConnected;
  final bool wifiConnected;
  final int rssi;

  // Firmware payload: V=<mV>;S=<0|1>;L=<0|1>;B=<0|1>;W=<0|1>
  static UpsStatus? fromAdvertisement({
    required String deviceId,
    required List<int> bytes,
    required int rssi,
    DateTime? receivedAt,
  }) {
    final text = utf8.decode(bytes, allowMalformed: true);
    final match = RegExp(r'V=(\d+);S=([01]);L=([01]);B=([01]);W=([01])').firstMatch(text);
    if (match == null) return null;
    return UpsStatus(
      deviceId: deviceId,
      receivedAt: receivedAt ?? DateTime.now(),
      voltageMv: int.parse(match.group(1)!),
      switchOn: match.group(2) == '1',
      lowBattery: match.group(3) == '1',
      bleConnected: match.group(4) == '1',
      wifiConnected: match.group(5) == '1',
      rssi: rssi,
    );
  }

  Map<String, Object> toJson() => {
        'deviceId': deviceId,
        'receivedAt': receivedAt.toIso8601String(),
        'voltageMv': voltageMv,
        'switchOn': switchOn,
        'lowBattery': lowBattery,
        'bleConnected': bleConnected,
        'wifiConnected': wifiConnected,
        'rssi': rssi,
      };

  factory UpsStatus.fromJson(Map<String, dynamic> json) => UpsStatus(
        deviceId: json['deviceId'] as String,
        receivedAt: DateTime.parse(json['receivedAt'] as String),
        voltageMv: json['voltageMv'] as int,
        switchOn: json['switchOn'] as bool,
        lowBattery: json['lowBattery'] as bool,
        bleConnected: json['bleConnected'] as bool,
        wifiConnected: json['wifiConnected'] as bool,
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
    await prefs.setString(_historyKey, jsonEncode(retained.map((entry) => entry.toJson()).toList()));
  }

  Future<void> clear() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_historyKey);
  }
}

class MonitorPage extends StatefulWidget {
  const MonitorPage({super.key});

  @override
  State<MonitorPage> createState() => _MonitorPageState();
}

class _MonitorPageState extends State<MonitorPage> {
  final _store = HistoryStore();
  final List<UpsStatus> _history = [];
  final Map<String, UpsStatus> _latestByDevice = {};
  StreamSubscription<List<ScanResult>>? _scanSubscription;
  String? _selectedDeviceId;
  String? _error;
  bool _scanning = false;

  UpsStatus? get _latest => _selectedDeviceId == null ? null : _latestByDevice[_selectedDeviceId];

  List<String> get _deviceIds => {..._history.map((entry) => entry.deviceId), ..._latestByDevice.keys}.toList()..sort();

  List<UpsStatus> get _visibleHistory => _selectedDeviceId == null
      ? _history
      : _history.where((entry) => entry.deviceId == _selectedDeviceId).toList();

  @override
  void initState() {
    super.initState();
    _loadAndStart();
  }

  Future<void> _loadAndStart() async {
    final stored = await _store.load();
    if (!mounted) return;
    setState(() {
      _history.addAll(stored);
      for (final entry in _history) {
        _latestByDevice[entry.deviceId] = entry;
      }
      if (_history.isNotEmpty) _selectedDeviceId = _history.last.deviceId;
    });
    await _startScan();
  }

  Future<void> _startScan() async {
    if (_scanning) return;
    try {
      if (!await FlutterBluePlus.isSupported) {
        setState(() => _error = 'Bluetooth LE is not supported on this device.');
        return;
      }
      await FlutterBluePlus.adapterState.where((state) => state == BluetoothAdapterState.on).first;
      _scanSubscription ??= FlutterBluePlus.onScanResults.listen(
        _handleResults,
        onError: (Object error) => mounted ? setState(() => _error = error.toString()) : null,
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
    final historicalForDevice = _history.where((entry) => entry.deviceId == received.deviceId).toList();
    final previousForDevice = historicalForDevice.isEmpty ? null : historicalForDevice.last;
    final stateChanged = previousForDevice == null ||
        previousForDevice.switchOn != received.switchOn ||
        previousForDevice.lowBattery != received.lowBattery ||
        previousForDevice.wifiConnected != received.wifiConnected ||
        previousForDevice.bleConnected != received.bleConnected;
    final shouldPersist = previousForDevice == null ||
        stateChanged ||
        received.receivedAt.difference(previousForDevice.receivedAt) >= _minimumPersistInterval;
    setState(() {
      _latestByDevice[received.deviceId] = received;
      _selectedDeviceId ??= received.deviceId;
      if (shouldPersist) _history.add(received);
    });
    if (shouldPersist) unawaited(_store.save(_history));
  }

  Future<void> _clearHistory() async {
    await _store.clear();
    if (mounted) setState(() {
      _history.clear();
      _latestByDevice.clear();
      _selectedDeviceId = null;
    });
  }

  @override
  void dispose() {
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
            icon: Icon(_scanning ? Icons.stop_circle_outlined : Icons.play_circle_outline),
            onPressed: _scanning ? _stopScan : _startScan,
          ),
          IconButton(tooltip: 'Clear history', icon: const Icon(Icons.delete_outline), onPressed: _clearHistory),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            if (_error != null) _ErrorBanner(message: _error!),
            if (_deviceIds.length > 1)
              DropdownButtonFormField<String>(
                value: _selectedDeviceId,
                decoration: const InputDecoration(labelText: 'Advertising device'),
                items: _deviceIds.map((id) => DropdownMenuItem(value: id, child: Text(id))).toList(),
                onChanged: (id) => setState(() => _selectedDeviceId = id),
              ),
            const SizedBox(height: 12),
            _StatusCard(status: latest, scanning: _scanning),
            const SizedBox(height: 20),
            Text('Voltage history', style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 8),
            SizedBox(height: 220, child: VoltageHistoryChart(samples: _visibleHistory)),
            const SizedBox(height: 16),
            Text('${_visibleHistory.length} stored samples', style: Theme.of(context).textTheme.bodySmall),
            const SizedBox(height: 8),
            ..._visibleHistory.reversed.take(12).map((entry) => _HistoryRow(status: entry)),
          ],
        ),
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({required this.status, required this.scanning});
  final UpsStatus? status;
  final bool scanning;

  @override
  Widget build(BuildContext context) {
    if (status == null) {
      return Card(child: Padding(padding: const EdgeInsets.all(20), child: Text(scanning ? 'Scanning for status advertisements…' : 'Scan is stopped.')));
    }
    final age = DateTime.now().difference(status!.receivedAt);
    return Card(
      color: status!.lowBattery ? Theme.of(context).colorScheme.errorContainer : null,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('${(status!.voltageMv / 1000).toStringAsFixed(3)} V', style: Theme.of(context).textTheme.displaySmall),
          const SizedBox(height: 12),
          Wrap(spacing: 8, runSpacing: 8, children: [
            _StateChip(label: status!.switchOn ? 'Switch ON' : 'Switch OFF', active: status!.switchOn),
            _StateChip(label: status!.lowBattery ? 'Battery LOW' : 'Battery OK', active: !status!.lowBattery),
            _StateChip(label: status!.wifiConnected ? 'Wi-Fi connected' : 'Wi-Fi off', active: status!.wifiConnected),
            _StateChip(label: status!.bleConnected ? 'BLE connected' : 'BLE advertising', active: status!.bleConnected),
          ]),
          const SizedBox(height: 12),
          Text('Last received ${_formatAge(age)} · RSSI ${status!.rssi} dBm'),
        ]),
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
        backgroundColor: active ? Theme.of(context).colorScheme.primaryContainer : null,
      );
}

class _HistoryRow extends StatelessWidget {
  const _HistoryRow({required this.status});
  final UpsStatus status;
  @override
  Widget build(BuildContext context) => ListTile(
        dense: true,
        contentPadding: EdgeInsets.zero,
        title: Text('${(status.voltageMv / 1000).toStringAsFixed(3)} V · ${status.lowBattery ? 'LOW' : 'OK'}'),
        subtitle: Text('${status.receivedAt.toLocal()} · switch ${status.switchOn ? 'ON' : 'OFF'}'),
        trailing: Text('${status.rssi} dBm'),
      );
}

class VoltageHistoryChart extends StatelessWidget {
  const VoltageHistoryChart({super.key, required this.samples});
  final List<UpsStatus> samples;
  @override
  Widget build(BuildContext context) => CustomPaint(
        painter: _VoltageChartPainter(samples, Theme.of(context).colorScheme),
        child: samples.length < 2 ? const Center(child: Text('Receive at least two samples to draw history.')) : null,
      );
}

class _VoltageChartPainter extends CustomPainter {
  const _VoltageChartPainter(this.samples, this.colors);
  final List<UpsStatus> samples;
  final ColorScheme colors;

  @override
  void paint(Canvas canvas, Size size) {
    if (samples.length < 2) return;
    const left = 42.0;
    const bottom = 24.0;
    final values = samples.map((entry) => entry.voltageMv.toDouble()).toList();
    final minValue = (values.reduce(math.min) - 100).floorToDouble();
    final maxValue = (values.reduce(math.max) + 100).ceilToDouble();
    final range = math.max(1.0, maxValue - minValue);
    final graph = Rect.fromLTWH(left, 8, size.width - left - 8, size.height - bottom - 8);
    final axis = Paint()..color = colors.outline;
    canvas.drawLine(Offset(graph.left, graph.top), Offset(graph.left, graph.bottom), axis);
    canvas.drawLine(Offset(graph.left, graph.bottom), Offset(graph.right, graph.bottom), axis);
    final path = Path();
    for (var i = 0; i < samples.length; i++) {
      final x = graph.left + graph.width * i / (samples.length - 1);
      final y = graph.bottom - graph.height * (values[i] - minValue) / range;
      if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    canvas.drawPath(path, Paint()..color = colors.primary..style = PaintingStyle.stroke..strokeWidth = 2);
    final labelPainter = TextPainter(textDirection: TextDirection.ltr);
    for (final value in [minValue, maxValue]) {
      labelPainter.text = TextSpan(text: '${(value / 1000).toStringAsFixed(2)} V', style: TextStyle(color: colors.onSurface, fontSize: 11));
      labelPainter.layout();
      final y = graph.bottom - graph.height * (value - minValue) / range;
      labelPainter.paint(canvas, Offset(0, y - 7));
    }
  }

  @override
  bool shouldRepaint(covariant _VoltageChartPainter oldDelegate) => oldDelegate.samples != samples || oldDelegate.colors != colors;
}

String _formatAge(Duration age) {
  if (age.inSeconds < 5) return 'just now';
  if (age.inMinutes < 1) return '${age.inSeconds}s ago';
  if (age.inHours < 1) return '${age.inMinutes}m ago';
  return '${age.inHours}h ago';
}
