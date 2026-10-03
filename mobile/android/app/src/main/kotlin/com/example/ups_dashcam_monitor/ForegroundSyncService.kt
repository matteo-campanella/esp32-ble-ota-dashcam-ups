package com.example.ups_dashcam_monitor

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattService
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Base64
import androidx.core.app.NotificationCompat
import java.nio.charset.StandardCharsets
import java.text.DateFormat
import java.util.Date
import java.util.UUID

/**
 * A short-lived foreground job. It waits for the ESP32's periodic advertising,
 * writes every configuration command with response, and stops immediately.
 */
class ForegroundSyncService : Service() {
    companion object {
        const val EXTRA_COMMANDS = "commands"
        const val EXTRA_DEVICE_ID = "deviceId"
        const val ACTION_CANCEL = "com.example.ups_dashcam_monitor.CANCEL_SYNC"
        private const val CHANNEL_ID = "ups_ble_sync"
        private const val NOTIFICATION_ID = 7102
        private const val COMPLETION_CHANNEL_ID = "ups_ble_sync_complete"
        private const val COMPLETION_NOTIFICATION_ID = 7103
        private const val WAIT_TIMEOUT_MS = 15 * 60 * 1000L
        private val SETTINGS_SERVICE_UUID: UUID = UUID.fromString("d96011fc-8ab0-42d9-93bb-ae202331297a")
        private val SETTINGS_UUID: UUID = UUID.fromString("235fefc9-58fd-4f84-977a-9a72ae348007")
        private val HISTORY_UUID: UUID = UUID.fromString("e6aa2d53-4ed4-43a6-a799-18dbf6a6d3da")
        private val HISTORY_PAGE_2_UUID: UUID = UUID.fromString("8e64f238-2ffc-4870-bd31-3358f3b5c82d")
        private val HISTORY_PAGE_3_UUID: UUID = UUID.fromString("9b90738b-a4d4-4b4d-91e1-9d0a114c8c4f")
        const val HISTORY_PREFERENCES = "ups_synced_history"
        const val HISTORY_DATA = "data"
        const val HISTORY_RECEIVED_AT = "receivedAt"
        const val RESULT_PREFERENCES = "ups_foreground_sync_result"
        const val RESULT_MESSAGE = "message"
        const val RESULT_SUCCESS = "success"
        const val RESULT_SAMPLE_COUNT = "sampleCount"
        @Volatile var isActive = false
    }

    private val handler = Handler(Looper.getMainLooper())
    private var scanner: BluetoothLeScanner? = null
    private var cpuWakeLock: PowerManager.WakeLock? = null
    private var gatt: BluetoothGatt? = null
    private var rxCharacteristic: BluetoothGattCharacteristic? = null
    private var historyCharacteristic: BluetoothGattCharacteristic? = null
    private var historyPage2Characteristic: BluetoothGattCharacteristic? = null
    private var historyPage3Characteristic: BluetoothGattCharacteristic? = null
    private var historyPage0 = ByteArray(0)
    private var historyPage1 = ByteArray(0)
    private var targetDeviceId = ""
    private var commands: List<String> = emptyList()
    private var nextCommand = 0
    private var scanning = false
    private var retryScheduled = false
    private var seenScanResults = 0
    private var finished = false

    private val timeout = Runnable { finish("ESP32 was not found before the sync timeout.") }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) return START_NOT_STICKY
        if (intent.action == ACTION_CANCEL) {
            finish("Synchronization cancelled.")
            return START_NOT_STICKY
        }
        isActive = true
        commands = intent.getStringArrayListExtra(EXTRA_COMMANDS) ?: emptyList()
        targetDeviceId = intent.getStringExtra(EXTRA_DEVICE_ID).orEmpty()
        startAsForeground("Waiting for bleUPS to wake…")
        acquireCpuWakeLock()
        if (commands.isEmpty() || targetDeviceId.isBlank()) {
            finish("Invalid synchronization request.")
        } else if (!hasBluetoothPermissions()) {
            finish("Bluetooth permission is not granted. Open the app and allow Nearby devices.")
        } else {
            startScan()
            handler.postDelayed(timeout, WAIT_TIMEOUT_MS)
        }
        return START_NOT_STICKY
    }

    private fun hasBluetoothPermissions(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        } else {
            checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun acquireCpuWakeLock() {
        if (cpuWakeLock?.isHeld == true) return
        val powerManager = getSystemService(PowerManager::class.java)
        cpuWakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "ups_dashcam_monitor:foreground_sync",
        ).apply {
            setReferenceCounted(false)
            // Safety timeout: the job itself has the same 15-minute bound.
            acquire(WAIT_TIMEOUT_MS + 5000L)
        }
    }

    private fun releaseCpuWakeLock() {
        cpuWakeLock?.let { wakeLock ->
            if (wakeLock.isHeld) wakeLock.release()
        }
        cpuWakeLock = null
    }

    private fun mergeHistoryPages(first: ByteArray, second: ByteArray, third: ByteArray): ByteArray? {
        fun recordBytes(page: ByteArray, expectedPage: Int): ByteArray? {
            if (page.size < 4 || page[0].toInt() != 1 || (page[2].toInt() and 0xFF) != expectedPage) {
                return null
            }
            val count = page[1].toInt() and 0xFF
            val length = 4 + count * 11
            return if (count <= 45 && page.size >= length) page.copyOfRange(4, length) else null
        }

        val firstRecords = recordBytes(first, 0) ?: return null
        val secondRecords = recordBytes(second, 1) ?: return null
        val thirdRecords = recordBytes(third, 2) ?: return null
        val count = (firstRecords.size + secondRecords.size + thirdRecords.size) / 11
        if (count > 100) return null
        return ByteArray(4 + firstRecords.size + secondRecords.size + thirdRecords.size).also { combined ->
            combined[0] = 1
            combined[1] = count.toByte()
            System.arraycopy(firstRecords, 0, combined, 4, firstRecords.size)
            System.arraycopy(secondRecords, 0, combined, 4 + firstRecords.size, secondRecords.size)
            System.arraycopy(
                thirdRecords,
                0,
                combined,
                4 + firstRecords.size + secondRecords.size,
                thirdRecords.size,
            )
        }
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        val adapter = BluetoothAdapter.getDefaultAdapter()
        if (adapter == null || !adapter.isEnabled) {
            finish("Bluetooth is turned off.")
            return
        }
        scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            finish("BLE scanning is unavailable.")
            return
        }
        // The ESP32 may advertise for only the short low-battery active window.
        // The platform default is low-power, duty-cycled scanning and can miss it.
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setReportDelay(0)
            .build()
        seenScanResults = 0
        scanning = true
        // Android suspends unfiltered BLE scans while the display is off. The
        // ESP32 always advertises with manufacturer identifier 0xFFFF.
        // A non-empty hardware filter keeps this bounded foreground scan active
        // with the phone locked; the callback still verifies the selected MAC.
        val espStatusFilter = ScanFilter.Builder().setManufacturerData(0xFFFF, null).build()
        scanner?.startScan(listOf(espStatusFilter), settings, scanCallback)
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (finished) return
            ++seenScanResults
            if (!sameDevice(result.device.address, targetDeviceId)) {
                if (seenScanResults % 50 == 0) {
                    updateNotification("Scanning nearby BLE devices; waiting for the selected ESP32…")
                }
                return
            }
            stopScan()
            updateNotification("Connecting to bleUPS…")
            gatt = result.device.connectGatt(this@ForegroundSyncService, false, gattCallback)
        }

        override fun onScanFailed(errorCode: Int) {
            finish("BLE scan failed ($errorCode).")
        }
    }

    private fun sameDevice(first: String, second: String): Boolean =
        first.filter { it.isLetterOrDigit() }.equals(
            second.filter { it.isLetterOrDigit() },
            ignoreCase = true,
        )

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (finished) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retryFromBleFailure("BLE connection failed ($status)")
            } else if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                updateNotification("Connected; discovering ESP32 services…")
                gatt.discoverServices()
            } else if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
                retryFromBleFailure("BLE disconnected before synchronization completed")
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retryFromBleFailure("Could not discover ESP32 BLE services ($status)")
                return
            }
            val service: BluetoothGattService? = gatt.getService(SETTINGS_SERVICE_UUID)
            rxCharacteristic = service?.getCharacteristic(SETTINGS_UUID)
            historyCharacteristic = service?.getCharacteristic(HISTORY_UUID)
            historyPage2Characteristic = service?.getCharacteristic(HISTORY_PAGE_2_UUID)
            historyPage3Characteristic = service?.getCharacteristic(HISTORY_PAGE_3_UUID)
            if (rxCharacteristic == null) {
                finish("The ESP32 Settings characteristic was not found.")
                return
            }
            updateNotification("Synchronizing settings…")
            writeNextCommand()
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retryFromBleFailure("Configuration write failed ($status)")
                return
            }
            writeNextCommand()
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            if (finished ||
                (characteristic.uuid != HISTORY_UUID &&
                    characteristic.uuid != HISTORY_PAGE_2_UUID &&
                    characteristic.uuid != HISTORY_PAGE_3_UUID)
            ) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                retryFromBleFailure("Voltage history read failed ($status)")
                return
            }
            if (characteristic.uuid == HISTORY_UUID) {
                historyPage0 = value
                val pageCount = if (value.size >= 4) value[3].toInt() and 0xFF else 1
                val secondPage = historyPage2Characteristic
                if (pageCount > 1 && secondPage != null) {
                    updateNotification("Retrieving remaining retained voltage history…")
                    if (!gatt.readCharacteristic(secondPage)) {
                        retryFromBleFailure("Remaining voltage history was unavailable")
                    }
                    return
                }
                persistHistory(value)
                return
            }
            if (characteristic.uuid == HISTORY_PAGE_2_UUID && historyPage0.size >= 4 &&
                (historyPage0[3].toInt() and 0xFF) > 2
            ) {
                historyPage1 = value
                val thirdPage = historyPage3Characteristic
                if (thirdPage == null || !gatt.readCharacteristic(thirdPage)) {
                    retryFromBleFailure("Final voltage history page was unavailable")
                }
                return
            }
            val combined = if (characteristic.uuid == HISTORY_PAGE_3_UUID) {
                mergeHistoryPages(historyPage0, historyPage1, value)
            } else {
                mergeHistoryPages(historyPage0, value)
            }
            if (combined == null) {
                retryFromBleFailure("Retained voltage history was malformed")
                return
            }
            persistHistory(combined)
        }

        private fun persistHistory(value: ByteArray) {
            getSharedPreferences(HISTORY_PREFERENCES, MODE_PRIVATE).edit()
                .putString(HISTORY_DATA, Base64.encodeToString(value, Base64.NO_WRAP))
                .putLong(HISTORY_RECEIVED_AT, System.currentTimeMillis() / 1000L)
                .apply()
            val count = if (value.size >= 2) value[1].toInt() and 0xFF else 0
            finish(
                if (count == 0) {
                    "Time synchronized. Samples will be available after the next wake cycle."
                } else {
                    "Settings and $count retained voltage samples synchronized."
                },
                success = true,
                sampleCount = count,
            )
        }
    }

    private fun mergeHistoryPages(first: ByteArray, second: ByteArray): ByteArray? {
        fun recordBytes(page: ByteArray, expectedPage: Int): ByteArray? {
            if (page.size < 4 || page[0].toInt() != 1 || (page[2].toInt() and 0xFF) != expectedPage) {
                return null
            }
            val count = page[1].toInt() and 0xFF
            val length = 4 + count * 11
            return if (count <= 45 && page.size >= length) page.copyOfRange(4, length) else null
        }

        val firstRecords = recordBytes(first, 0) ?: return null
        val secondRecords = recordBytes(second, 1) ?: return null
        val count = (firstRecords.size + secondRecords.size) / 11
        if (count > 100) return null
        return ByteArray(4 + firstRecords.size + secondRecords.size).also { combined ->
            combined[0] = 1
            combined[1] = count.toByte()
            System.arraycopy(firstRecords, 0, combined, 4, firstRecords.size)
            System.arraycopy(secondRecords, 0, combined, 4 + firstRecords.size, secondRecords.size)
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeNextCommand() {
        if (finished) return
        if (nextCommand >= commands.size) {
            val history = historyCharacteristic
            historyPage0 = ByteArray(0)
            historyPage1 = ByteArray(0)
            if (history == null || !gatt!!.readCharacteristic(history)) {
                retryFromBleFailure("Voltage history was unavailable")
            } else {
                updateNotification("Retrieving retained voltage history…")
            }
            return
        }
        val characteristic = rxCharacteristic ?: run {
            finish("BLE write characteristic is unavailable.")
            return
        }
        val command = commands[nextCommand++]
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        characteristic.value = command.toByteArray(StandardCharsets.UTF_8)
        if (!gatt!!.writeCharacteristic(characteristic)) {
            retryFromBleFailure("Could not send configuration to the ESP32")
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (scanning) scanner?.stopScan(scanCallback)
        scanning = false
    }

    @SuppressLint("MissingPermission")
    private fun retryFromBleFailure(reason: String) {
        if (finished || retryScheduled) return
        retryScheduled = true
        updateNotification("$reason; waiting for the next ESP32 advertisement…")
        stopScan()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxCharacteristic = null
        historyCharacteristic = null
        historyPage2Characteristic = null
        historyPage3Characteristic = null
        historyPage0 = ByteArray(0)
        historyPage1 = ByteArray(0)
        nextCommand = 0
        handler.postDelayed({
            retryScheduled = false
            if (!finished) startScan()
        }, 1000L)
    }

    @SuppressLint("MissingPermission")
    private fun finish(message: String, success: Boolean = false, sampleCount: Int = 0) {
        if (finished) return
        finished = true
        isActive = false
        retryScheduled = false
        getSharedPreferences(RESULT_PREFERENCES, MODE_PRIVATE).edit()
            .putString(RESULT_MESSAGE, message)
            .putBoolean(RESULT_SUCCESS, success)
            .putInt(RESULT_SAMPLE_COUNT, sampleCount)
            .apply()
        releaseCpuWakeLock()
        handler.removeCallbacks(timeout)
        stopScan()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        updateNotification(message)
        if (success) postSuccessfulCompletion(message)
        handler.postDelayed({ stopForeground(STOP_FOREGROUND_REMOVE); stopSelf() }, 1500)
    }

    private fun startAsForeground(message: String) {
        createChannel()
        val notification = notification(message)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification(message: String) {
        val manager = getSystemService(NotificationManager::class.java)
        manager.notify(NOTIFICATION_ID, notification(message))
    }

    private fun notification(message: String): Notification = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
        .setContentTitle("UPS Dashcam synchronization")
        .setContentText(message)
        .setOngoing(!finished)
        .setOnlyAlertOnce(true)
        .build()

    private fun postSuccessfulCompletion(message: String) {
        val completedAt = DateFormat.getDateTimeInstance(
            DateFormat.MEDIUM,
            DateFormat.SHORT,
        ).format(Date())
        val notification = NotificationCompat.Builder(this, COMPLETION_CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("UPS Dashcam synchronized")
            .setContentText("$completedAt — $message")
            .setStyle(NotificationCompat.BigTextStyle().bigText("$completedAt — $message"))
            .setAutoCancel(true)
            .build()
        getSystemService(NotificationManager::class.java)
            .notify(COMPLETION_NOTIFICATION_ID, notification)
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(CHANNEL_ID, "UPS BLE synchronization", NotificationManager.IMPORTANCE_LOW)
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
            val completionChannel = NotificationChannel(
                COMPLETION_CHANNEL_ID,
                "UPS BLE synchronization results",
                NotificationManager.IMPORTANCE_DEFAULT,
            )
            getSystemService(NotificationManager::class.java)
                .createNotificationChannel(completionChannel)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        isActive = false
        releaseCpuWakeLock()
        handler.removeCallbacks(timeout)
        stopScan()
        gatt?.close()
        super.onDestroy()
    }
}
