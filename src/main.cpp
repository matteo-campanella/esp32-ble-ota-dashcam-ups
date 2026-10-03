#include "main.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

Logger logger;
Leds leds;
BLEData bleData;

// Logger uses this flag to avoid writes to BLE after its stack was stopped.
bool isModemSleepOn = true;
RTC_DATA_ATTR bool lowBatteryState = false;
RTC_DATA_ATTR bool veryLowBatteryState = false;

namespace {
enum class RuntimeState : uint8_t { Measure, ConnectionWindow };

struct VoltageAccumulator {
    uint32_t sum = 0;
    uint32_t externalSupplySum = 0;
    uint8_t count = 0;
    uint8_t externalSupplyCount = 0;
    unsigned long startedAt = 0;
    unsigned long lastSampleAt = 0;

    void reset() {
        sum = 0;
        externalSupplySum = 0;
        count = 0;
        externalSupplyCount = 0;
        startedAt = millis();
        lastSampleAt = 0;
    }

    // A valid result exists only after this complete moving-average window.
    bool update(uint16_t &millivolts, uint16_t &externalSupplyMillivolts) {
        const unsigned long now = millis();
        if (lastSampleAt != 0 && now - lastSampleAt < BATTERY_VALIDATION_SAMPLE_MS) return false;
        lastSampleAt = now;

        // A zero external reading is valid: it means the external supply is
        // absent. It is collected independently of battery validity.
        const uint32_t externalAdcMillivolts = analogReadMilliVolts(EXTERNAL_SUPPLY_ADC_PIN);
        externalSupplySum += (externalAdcMillivolts * EXTERNAL_SUPPLY_CALIBRATION_X1000 + 500UL) /
                             1000UL;
        ++externalSupplyCount;

        const uint16_t raw = analogRead(ADC_PIN);
        if (raw == 0) return false;

        // Use the Arduino ADC calibration for GPIO35, then apply the fixed
        // battery calibration scale to obtain battery millivolts.
        const uint32_t adcMillivolts = analogReadMilliVolts(ADC_PIN);
        if (adcMillivolts == 0) return false;
        sum += (adcMillivolts * BATTERY_CALIBRATION_X1000 + 500UL) / 1000UL;

        ++count;
        if (count < BATTERY_VALIDATION_SAMPLES) return false;

        millivolts = static_cast<uint16_t>(sum / count);
        externalSupplyMillivolts = externalSupplyMillivoltsAverage();
        return true;
    }

    bool timedOut() const { return millis() - startedAt >= BATTERY_VALIDATION_TIMEOUT_MS; }

    uint16_t externalSupplyMillivoltsAverage() const {
        return externalSupplyCount == 0 ? 0
                                        : static_cast<uint16_t>(externalSupplySum / externalSupplyCount);
    }
};

RuntimeState state = RuntimeState::Measure;
VoltageAccumulator voltageSamples;
unsigned long connectionWindowStartedAt = 0;
bool wifiScanStarted = false;
bool wifiConnectStarted = false;
bool wifiListening = false;
bool lastBleConnectionState = false;

bool isExternalSupplyHigh() {
    return bleData.externalSupplyVoltage > deviceConfiguration.externalSupplyHighMillivolts;
}

uint64_t sleepIntervalUs() {
    // Battery protection always wins. A disconnected or otherwise invalid
    // supply-sense input must never make a depleted battery wake more often.
    uint32_t seconds = veryLowBatteryState
                           ? VERY_LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC
                           : (lowBatteryState ? LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC
                                              : (isExternalSupplyHigh()
                                                     ? EXTERNAL_SUPPLY_DEEP_SLEEP_INTERVAL_SEC
                                                     : DEEP_SLEEP_INTERVAL_SEC));
    // A calendar boundary cannot enable the load while battery protection is
    // active, so it must not shorten a protective sleep interval.
    if (!lowBatteryState) {
        const uint32_t transitionSeconds = configuration_seconds_until_transition();
        if (transitionSeconds < seconds) seconds = transitionSeconds;
    }
    if (seconds == 0) seconds = 1;
    return seconds * 1000000ULL;
}

unsigned long connectionWindowMs() {
    const unsigned long seconds = veryLowBatteryState
                                      ? VERY_LOW_BATTERY_CONNECTION_WINDOW_SEC
                                      : (lowBatteryState ? LOW_BATTERY_CONNECTION_WINDOW_SEC
                                                         : (isExternalSupplyHigh()
                                                                ? EXTERNAL_SUPPLY_CONNECTION_WINDOW_SEC
                                                                : CONNECTION_WINDOW_SEC));
    return seconds * 1000UL;
}

void switchOn() { digitalWrite(SWITCH_PIN, HIGH); }
void switchOff() { digitalWrite(SWITCH_PIN, LOW); }

void applyLoadPolicy() {
    // A forced ON request only overrides the weekly calendar. It never defeats
    // low-battery protection or an invalid measurement.
    if (lowBatteryState || !configuration_load_is_allowed()) switchOff();
    else switchOn();
}

unsigned long activeConnectionWindowMs() {
    unsigned long window = connectionWindowMs();
    // Very-low battery deliberately does not grant an extended BLE session:
    // preserving the cell takes precedence over configuration convenience.
    if (ble_is_connected() && !veryLowBatteryState) {
        const unsigned long connectedWindow = BLE_CONNECTED_WINDOW_SEC * 1000UL;
        if (connectedWindow > window) window = connectedWindow;
    }
    return window;
}

void applyBatteryState(uint16_t millivolts, bool measurementValid) {
    if (!measurementValid) {
        // An unknown voltage must never turn the protected load on.
        lowBatteryState = true;
        veryLowBatteryState = true;
        applyLoadPolicy();
        logger.println("Battery measurement timed out: 0.000 V; load kept off.");
        return;
    }

    const bool wasLow = lowBatteryState;
    const bool wasVeryLow = veryLowBatteryState;
    if (millivolts < deviceConfiguration.veryLowBatteryMillivolts) {
        veryLowBatteryState = true;
        lowBatteryState = true;
    } else if (millivolts < deviceConfiguration.lowBatteryMillivolts) {
        veryLowBatteryState = false;
        lowBatteryState = true;
    } else if (lowBatteryState && millivolts >= deviceConfiguration.recoveryMillivolts) {
        veryLowBatteryState = false;
        lowBatteryState = false;
    } else if (veryLowBatteryState) {
        // Above the very-low boundary but still below regular recovery: the
        // device remains low, using the less severe low-battery timing.
        veryLowBatteryState = false;
    }

    applyLoadPolicy();
    if (lowBatteryState) {
        if (!wasLow || wasVeryLow != veryLowBatteryState) {
            logger.printfln("LOWBAT%s V=%u", veryLowBatteryState ? " VERY" : "", millivolts);
        }
    } else {
        // This includes the hysteresis band when the previous state was OK.
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
    ble_advertise_status(bleData.voltage, bleData.externalSupplyVoltage,
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
    configuration_prepare_sleep(static_cast<uint32_t>(intervalUs / 1000000ULL));
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
    logger.printf("ExternalSupply: %u\n", bleData.externalSupplyVoltage);
    logger.printf("ExternalSupplyHighTrigger: %u\n",
                  deviceConfiguration.externalSupplyHighMillivolts);
    logger.printf("ExternalSupplyHigh: %s\n", isExternalSupplyHigh() ? "YES" : "NO");
    logger.printf("Status: %s\n", digitalRead(SWITCH_PIN) ? "ON" : "OFF");
    logger.printf("LowBattery: %s\n", lowBatteryState ? "YES" : "NO");
    logger.printf("VeryLowBattery: %s\n", veryLowBatteryState ? "YES" : "NO");
    logger.printf("BLE: %s\n", ble_is_connected() ? "CONNECTED" : "OFF");
    logger.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFF");
    logger.printf("Override: %u\n", static_cast<uint8_t>(deviceConfiguration.overrideMode));
    logger.printf("Calendar: %s\n", deviceConfiguration.calendarEnabled ? "ON" : "OFF");
    logger.printf("Battery thresholds: very-low=%u low=%u recovery=%u mV\n",
                  deviceConfiguration.veryLowBatteryMillivolts,
                  deviceConfiguration.lowBatteryMillivolts,
                  deviceConfiguration.recoveryMillivolts);
}

void checkIncomingCommands() {
    String command = ble_uart_receive();
    if (command.length() == 0) command = logger.udpReceive();
    if (Serial.available() > 0) command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() == 0) return;

    String configurationResponse;
    if (configuration_handle_command(command, configurationResponse)) {
        // Threshold, override, calendar, and time changes all apply in this
        // active BLE window. Re-evaluate battery hysteresis when possible.
        if (bleData.voltage != 0) applyBatteryState(bleData.voltage, true);
        else applyLoadPolicy();
        logger.println(configurationResponse.c_str());
        ble_refresh_history();
        refreshAdvertisedStatus();
        return;
    }

    if (command == "r" || command == "reset") {
        logger.println("Restarting ESP...");
        Serial.flush();
        ESP.restart();
    } else if (command == "s" || command == "sleep") {
        enterDeepSleep();
    } else if (command == "m" || command == "dump") {
        printStatus();
    } else if (command == "h") {
        logger.printfln("History samples: %u/%u", history_count(), RTC_HISTORY_CAPACITY);
    }
}

void startConnectionWindow() {
    applyBatteryState(bleData.voltage, bleData.voltage != 0);
    history_append(bleData.voltage, lowBatteryState, digitalRead(SWITCH_PIN) == HIGH,
                   bleData.voltage != 0, configuration_now_epoch());
    logger.printfln("Battery measurement: %u.%03u V", bleData.voltage / 1000U,
                    bleData.voltage % 1000U);
    logger.printfln("External supply measurement: %u.%03u V",
                    bleData.externalSupplyVoltage / 1000U,
                    bleData.externalSupplyVoltage % 1000U);
    connectionWindowStartedAt = millis();
    startRadios();
    refreshAdvertisedStatus();
    lastBleConnectionState = ble_is_connected();
    leds.setBlinkMode(Leds::blink_slow);
    logger.printfln("Connection window started. V=%u low=%s very-low=%s ext-high=%s", bleData.voltage,
                    lowBatteryState ? "YES" : "NO",
                    veryLowBatteryState ? "YES" : "NO",
                    isExternalSupplyHigh() ? "YES" : "NO");
    state = RuntimeState::ConnectionWindow;
}
} // namespace

void setup() {
    const bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    Serial.begin(115200);
    Serial.println();
    Serial.flush();
    configuration_begin(timerWake);
    history_begin();
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
            uint16_t externalSupplyMillivolts = 0;
            if (voltageSamples.update(millivolts, externalSupplyMillivolts)) {
                bleData.voltage = millivolts;
                bleData.externalSupplyVoltage = externalSupplyMillivolts;
                startConnectionWindow();
            } else if (voltageSamples.timedOut()) {
                bleData.voltage = 0;
                bleData.externalSupplyVoltage = voltageSamples.externalSupplyMillivoltsAverage();
                startConnectionWindow();
            }
            break;
        }
        case RuntimeState::ConnectionWindow:
            checkIncomingCommands();
            // A sleep command resumes here only after wake, in Measure state.
            if (state != RuntimeState::ConnectionWindow) break;
            // A calendar boundary can occur while the connection window is
            // open; apply it immediately instead of waiting for the next wake.
            applyLoadPolicy();
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
                // A successful connection earns a complete configuration
                // window. This is especially necessary when the low-battery
                // idle window is intentionally very short.
                if (bleConnectionState) connectionWindowStartedAt = millis();
                refreshAdvertisedStatus();
                if (bleConnectionState) ble_update(&bleData);
            }
            if (activeConnectionWindowMs() == 0 || millis() - connectionWindowStartedAt >= activeConnectionWindowMs()) {
                logger.printfln("Cycle complete; battery=%s external=%s",
                                veryLowBatteryState ? "VERY LOW" : (lowBatteryState ? "LOW" : "OK"),
                                isExternalSupplyHigh() ? "HIGH" : "LOW");
                enterDeepSleep();
            }
            break;
    }
    delay(10);
}
