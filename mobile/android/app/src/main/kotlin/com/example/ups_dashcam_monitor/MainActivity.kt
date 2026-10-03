package com.example.ups_dashcam_monitor

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val syncChannel = "ups_dashcam_monitor/foreground_sync"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, syncChannel)
            .setMethodCallHandler { call, result ->
                if (call.method == "getSyncedHistory") {
                    val preferences = getSharedPreferences(ForegroundSyncService.HISTORY_PREFERENCES, MODE_PRIVATE)
                    result.success(mapOf(
                        "data" to preferences.getString(ForegroundSyncService.HISTORY_DATA, ""),
                        "receivedAt" to preferences.getLong(ForegroundSyncService.HISTORY_RECEIVED_AT, 0L),
                    ))
                    return@setMethodCallHandler
                }
                if (call.method == "clearSyncedHistory") {
                    getSharedPreferences(ForegroundSyncService.HISTORY_PREFERENCES, MODE_PRIVATE)
                        .edit()
                        .clear()
                        .apply()
                    result.success(null)
                    return@setMethodCallHandler
                }
                if (call.method == "isForegroundSyncActive") {
                    result.success(ForegroundSyncService.isActive)
                    return@setMethodCallHandler
                }
                if (call.method == "getForegroundSyncResult") {
                    val preferences = getSharedPreferences(ForegroundSyncService.RESULT_PREFERENCES, MODE_PRIVATE)
                    result.success(mapOf(
                        "message" to preferences.getString(ForegroundSyncService.RESULT_MESSAGE, "Synchronization ended."),
                        "success" to preferences.getBoolean(ForegroundSyncService.RESULT_SUCCESS, false),
                        "sampleCount" to preferences.getInt(ForegroundSyncService.RESULT_SAMPLE_COUNT, 0),
                    ))
                    return@setMethodCallHandler
                }
                if (call.method == "cancelForegroundSync") {
                    if (!ForegroundSyncService.isActive) {
                        result.success(null)
                        return@setMethodCallHandler
                    }
                    val intent = Intent(this, ForegroundSyncService::class.java).apply {
                        action = ForegroundSyncService.ACTION_CANCEL
                    }
                    ContextCompat.startForegroundService(this, intent)
                    result.success(null)
                    return@setMethodCallHandler
                }
                if (call.method != "startForegroundSync") {
                    result.notImplemented()
                    return@setMethodCallHandler
                }
                val commands = call.argument<List<String>>("commands")?.let { ArrayList(it) }
                val deviceId = call.argument<String>("deviceId")
                if (commands.isNullOrEmpty() || deviceId.isNullOrBlank()) {
                    result.error("invalid_arguments", "A device ID and BLE commands are required.", null)
                    return@setMethodCallHandler
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 7102)
                    result.error(
                        "notification_permission_required",
                        "Allow notifications, then tap Sync to ESP32 again.",
                        null,
                    )
                    return@setMethodCallHandler
                }
                val intent = Intent(this, ForegroundSyncService::class.java).apply {
                    putStringArrayListExtra(ForegroundSyncService.EXTRA_COMMANDS, commands)
                    putExtra(ForegroundSyncService.EXTRA_DEVICE_ID, deviceId)
                }
                ContextCompat.startForegroundService(this, intent)
                result.success(null)
            }
    }
}
