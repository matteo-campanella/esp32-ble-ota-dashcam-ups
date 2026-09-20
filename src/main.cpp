#include "main.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

Logger logger;
Leds leds;
BLEData bleData;

// Logger uses this flag to avoid writes to BLE after its stack was stopped.
bool isModemSleepOn = true;
RTC_DATA_ATTR bool lowBatteryState = false;

namespace {
enum class RuntimeState : uint8_t { Measure, ConnectionWindow };

struct VoltageAccumulator {
    uint32_t sum = 0;
    uint8_t count = 0;
    unsigned long startedAt = 0;
    unsigned long lastSampleAt = 0;

    void reset() {
        sum = 0;
        count = 0;
        startedAt = millis();
        lastSampleAt = 0;
    }

    // A valid result exists only after this complete moving-average window.
    bool update(uint16_t &millivolts) {
        const unsigned long now = millis();
        if (lastSampleAt != 0 && now - lastSampleAt < BATTERY_VALIDATION_SAMPLE_MS) return false;
        lastSampleAt = now;

        const uint16_t raw = analogRead(ADC_PIN);
        if (raw == 0) return false;

        // Use the Arduino ADC calibration for the voltage at GPIO35, then
        // undo the external 300k/91k divider to obtain battery millivolts.
        const uint32_t adcMillivolts = analogReadMilliVolts(ADC_PIN);
        if (adcMillivolts == 0) return false;
        sum += (adcMillivolts * BATTERY_DIVIDER_NUMERATOR) /
               BATTERY_DIVIDER_BOTTOM_OHMS;
        ++count;
        if (count < BATTERY_VALIDATION_SAMPLES) return false;

        millivolts = static_cast<uint16_t>(sum / count);
        return true;
    }

    bool timedOut() const { return millis() - startedAt >= BATTERY_VALIDATION_TIMEOUT_MS; }
};

RuntimeState state = RuntimeState::Measure;
VoltageAccumulator voltageSamples;
unsigned long connectionWindowStartedAt = 0;
bool wifiScanStarted = false;
bool wifiConnectStarted = false;
bool wifiListening = false;
bool lastBleConnectionState = false;

uint64_t sleepIntervalUs() {
    const uint64_t seconds = lowBatteryState ? LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC
                                             : DEEP_SLEEP_INTERVAL_SEC;
    return seconds * 1000000ULL;
}

unsigned long connectionWindowMs() {
    const unsigned long seconds = lowBatteryState ? LOW_BATTERY_CONNECTION_WINDOW_SEC
                                                  : CONNECTION_WINDOW_SEC;
    return seconds * 1000UL;
}

void switchOn() { digitalWrite(SWITCH_PIN, HIGH); }
void switchOff() { digitalWrite(SWITCH_PIN, LOW); }

void applyBatteryState(uint16_t millivolts, bool measurementValid) {
    if (!measurementValid) {
        // An unknown voltage must never turn the protected load on.
        lowBatteryState = true;
        switchOff();
        logger.println("Battery measurement timed out: 0.000 V; load kept off.");
        return;
    }

    const bool wasLow = lowBatteryState;
    if (millivolts < LOW_BATTERY_VOLTAGE_THRESHOLD) {
        lowBatteryState = true;
    } else if (lowBatteryState && millivolts >= BATTERY_RECOVERY_VOLTAGE_THRESHOLD) {
        lowBatteryState = false;
    }

    if (lowBatteryState) {
        switchOff();
        if (!wasLow) logger.printfln("LOWBAT V=%u", millivolts);
    } else {
        // This includes the hysteresis band when the previous state was OK.
        switchOn();
        if (wasLow) logger.printfln("BATTERY OK V=%u", millivolts);
    }
}

void startWifiConnection() {
    if (!USE_WIFI || wifiScanStarted) return;
    WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(true); // asynchronous: it cannot consume the connection window by blocking
    wifiScanStarted = true;
}

void serviceWifiConnection() {
    if (!USE_WIFI || !wifiScanStarted || wifiConnectStarted) return;

    const int networkCount = WiFi.scanComplete();
    if (networkCount == WIFI_SCAN_RUNNING || networkCount == WIFI_SCAN_FAILED) return;

    for (int network = 0; network < networkCount && !wifiConnectStarted; ++network) {
        for (int credential = 0; WIFI_CREDENTIALS[credential][0] != nullptr; ++credential) {
            if (WiFi.SSID(network) == WIFI_CREDENTIALS[credential][0]) {
                WiFi.begin(WIFI_CREDENTIALS[credential][0], WIFI_CREDENTIALS[credential][1]);
                wifiConnectStarted = true;
                logger.printfln("WiFi connection started: %s", WIFI_CREDENTIALS[credential][0]);
                break;
            }
        }
    }
    WiFi.scanDelete();
}

void startRadios() {
    ble_setup();
    isModemSleepOn = false;
    startWifiConnection();
}

void refreshAdvertisedStatus() {
    ble_advertise_status(bleData.voltage,
                         digitalRead(SWITCH_PIN) == HIGH,
                         lowBatteryState,
                         WiFi.status() == WL_CONNECTED);
}

void stopRadios() {
    // Set this first: subsequent logging is serial-only during BLE teardown.
    isModemSleepOn = true;
    ble_stop();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    wifiScanStarted = false;
    wifiConnectStarted = false;
    wifiListening = false;
}

[[noreturn]] void enterDeepSleep() {
    const uint64_t intervalUs = sleepIntervalUs();
    stopRadios();
    leds.setBlinkMode(Leds::blink_off);
    digitalWrite(RED_LED, LOW);

    // GPIO33 is output-capable and RTC-capable. Latch its current output level
    // so the external switch remains in its battery-selected state during the
    // reset caused by deep sleep.
    if (gpio_hold_en(static_cast<gpio_num_t>(SWITCH_PIN)) != ESP_OK) {
        logger.println("ERROR: switch GPIO hold could not be enabled.");
    }
    gpio_deep_sleep_hold_en();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(intervalUs);
    logger.printfln("Entering deep sleep for %llu s", intervalUs / 1000000ULL);
    Serial.flush();
    delay(20);
    esp_deep_sleep_start();
    while (true) delay(1000); // esp_deep_sleep_start() does not return.
}

void printStatus() {
    logger.printf("\nV: %u\n", bleData.voltage);
    logger.printf("Status: %s\n", digitalRead(SWITCH_PIN) ? "ON" : "OFF");
    logger.printf("LowBattery: %s\n", lowBatteryState ? "YES" : "NO");
    logger.printf("BLE: %s\n", ble_is_connected() ? "CONNECTED" : "OFF");
    logger.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFF");
}

void checkIncomingCommands() {
    String command = ble_uart_receive();
    if (command.length() == 0) command = logger.udpReceive();
    if (Serial.available() > 0) command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() == 0) return;

    if (command == "r" || command == "reset") {
        logger.println("Restarting ESP...");
        Serial.flush();
        ESP.restart();
    } else if (command == "s" || command == "sleep") {
        enterDeepSleep();
    } else if (command == "m" || command == "dump") {
        printStatus();
    }
}

void startConnectionWindow() {
    applyBatteryState(bleData.voltage, bleData.voltage != 0);
    logger.printfln("Battery measurement: %u.%03u V", bleData.voltage / 1000U,
                    bleData.voltage % 1000U);
    connectionWindowStartedAt = millis();
    startRadios();
    refreshAdvertisedStatus();
    lastBleConnectionState = ble_is_connected();
    leds.setBlinkMode(Leds::blink_slow);
    logger.printfln("Connection window started. V=%u low=%s", bleData.voltage,
                    lowBatteryState ? "YES" : "NO");
    state = RuntimeState::ConnectionWindow;
}
} // namespace

void setup() {
    const bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    Serial.begin(115200);
    Serial.println();
    Serial.flush();
    pinMode(SWITCH_PIN, OUTPUT);
    if (timerWake) {
        // Configure the reset GPIO logic while the pad is still held, then
        // release it: this avoids an OFF/ON glitch at the external switch.
        digitalWrite(SWITCH_PIN, lowBatteryState ? LOW : HIGH);
        gpio_hold_dis(static_cast<gpio_num_t>(SWITCH_PIN));
        gpio_deep_sleep_hold_dis();
    } else {
        // A cold boot has no trusted retained decision, so begin safely off.
        gpio_hold_dis(static_cast<gpio_num_t>(SWITCH_PIN));
        gpio_deep_sleep_hold_dis();
        switchOff();
    }

    // OTA is a cold-boot maintenance operation. A timer wake is the normal
    // low-power cycle and must never spend energy checking for updates.
    if (!timerWake) {
        logger.println("Cold boot: checking OTA update.");
        ota_setup();
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
    }

    analogReadResolution(12);
    // 0 dB is the most sensitive setting. The divider produces about 0.978 V
    // from a fully charged 4.20 V Li-ion battery.
    analogSetAttenuation(ADC_0db);
    leds.setup();
    leds.setBlinkMode(Leds::blink_fast);
    voltageSamples.reset();
    logger.printfln("Boot; wake cause=%d", esp_sleep_get_wakeup_cause());
}

void loop() {
    switch (state) {
        case RuntimeState::Measure: {
            uint16_t millivolts = 0;
            if (voltageSamples.update(millivolts)) {
                bleData.voltage = millivolts;
                startConnectionWindow();
            } else if (voltageSamples.timedOut()) {
                bleData.voltage = 0;
                startConnectionWindow();
            }
            break;
        }
        case RuntimeState::ConnectionWindow:
            checkIncomingCommands();
            // A sleep command resumes here only after wake, in Measure state.
            if (state != RuntimeState::ConnectionWindow) break;
            serviceWifiConnection();
            if (!wifiListening && WiFi.status() == WL_CONNECTED) {
                logger.udpListen();
                wifiListening = true;
            }
            // The voltage and switch state are constant for this entire
            // window. Reconfiguring a running Bluedroid advertiser every
            // 250 ms can block its host task, so update its payload only when
            // the one field that can change here (BLE connection state) does.
            const bool bleConnectionState = ble_is_connected();
            if (bleConnectionState != lastBleConnectionState) {
                lastBleConnectionState = bleConnectionState;
                refreshAdvertisedStatus();
                if (bleConnectionState) ble_update(&bleData);
            }
            if (connectionWindowMs() == 0 || millis() - connectionWindowStartedAt >= connectionWindowMs()) {
                logger.printfln("Cycle complete; battery=%s", lowBatteryState ? "LOW" : "OK");
                enterDeepSleep();
            }
            break;
    }
    delay(10);
}
