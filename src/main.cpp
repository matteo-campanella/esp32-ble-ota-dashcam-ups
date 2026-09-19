#include "main.h"
#include <esp_bt.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

Logger logger;
Leds leds;
BLEData bleData;

bool debug = false;
bool isModemSleepOn = false;
bool isGoToSleep = false;
bool isWakeUp = false;
bool timerWakeUp = false;
bool switchStatus = false;
RTC_DATA_ATTR bool lowBatteryState = false;
movingAvg voltage(10);

String command;
TaskHandle_t commTask,sensorsTask;
unsigned long wakeActiveStartedAt = 0;

void wifi_off() {
    WiFi.disconnect(true,false);
    WiFi.mode(WIFI_OFF);
    leds.wifiStatus=Leds::WIFISTATUS::wifi_off;
    logger.print("NET-");
}

bool wifi_connect() {
    const char *found_ssid = NULL;
    int n = 0;

    for (int i = 0; i < 3; i++) {
        n = WiFi.scanNetworks();
        if (n > 0) break;
        delay(250);
    }

    for (int i = 0; i < n; ++i) {
        int j = 0;
        while (WIFI_CREDENTIALS[j][0] != NULL) {
            if (WiFi.SSID(i) == WIFI_CREDENTIALS[j][0]) {
                found_ssid = WIFI_CREDENTIALS[j][0];
                const char *passphrase = WIFI_CREDENTIALS[j][1];
                WiFi.begin(found_ssid, passphrase);
                break;
            }
            j++;
        }
    }

    if (found_ssid == NULL) {
        logger.println("No known WiFi found for OTA.");
        wifi_off();
        return false;
    }

    logger.printfln("Connecting to WiFi: %s ...", found_ssid);
    WiFi.mode(WIFI_STA);
    leds.wifiStatus=Leds::WIFISTATUS::wifi_on;

    int tries = 50;
    while (WiFi.status() != WL_CONNECTED && tries > 0) {
        delay(250);
        tries--;
    }

    if (tries == 0) {
        logger.println("Failed to connect to WiFi for OTA!");
        wifi_off();
        return false;
    }

    leds.wifiStatus=Leds::WIFISTATUS::wifi_connected;
    logger.print("NET+");
    return true;
}

void wifi_setup() {
    if (USE_WIFI) wifi_connect();
    else wifi_off();
}

void modem_sleep() {
    ble_stop();
    btStop();
    //WiFi.disconnect();
    //WiFi.setSleep(true);
    //WiFi.mode(WIFI_OFF);
    //setCpuFrequencyMhz(40);
    isModemSleepOn = true;
}

void modem_awake() {
    isModemSleepOn = false;
    //setCpuFrequencyMhz(240);
    btStart();
    ble_setup();
}

static uint64_t get_deep_sleep_interval_us() {
    const uint64_t intervalSec = lowBatteryState ? LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC : DEEP_SLEEP_INTERVAL_SEC;
    return intervalSec * 1000000ULL;
}

void esp_deep_sleep() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(get_deep_sleep_interval_us());
    logger.printfln("Entering Deep Sleep... interval=%llu s", lowBatteryState ? LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC : DEEP_SLEEP_INTERVAL_SEC);
    delay(1000);
    esp_deep_sleep_start();
}

void check_incoming_commands() {
    command = ble_uart_receive();
    if (Serial.available()>0) command = Serial.readStringUntil('\n');
    // command = logger.udpReceive();
    if (command.length()==0) return;
    command.trim();
    if (command == "d" || command == "debug") {
        debug = !debug;
        logger.printfln("Debug %s...", debug ? "ON" : "OFF");
    } 
    else if (command == "r" || command == "reset") {
        logger.println("Restarting ESP...");
        delay(1000);
        ESP.restart();
    } 
    else if (command == "s" || command == "sleep") {
        logger.println("Entering Sleep...");
        esp_deep_sleep();
    }   
    else if (command == "u" || command == "upload") {
        logger.println("Uploading Log...");
        //TODO implement upload
    }
    else if (command == "m" || command == "dump") {
        const char *btState = "OFF";
        const char *wifiState = "OFF";

        switch (Leds::btStatus) {
            case Leds::BTSTATUS::bt_on: btState = "ON"; break;
            case Leds::BTSTATUS::bt_connected: btState = "CONNECTED"; break;
            case Leds::BTSTATUS::bt_off: btState = "OFF"; break;
        }

        switch (Leds::wifiStatus) {
            case Leds::WIFISTATUS::wifi_on: wifiState = "ON"; break;
            case Leds::WIFISTATUS::wifi_connected: wifiState = "CONNECTED"; break;
            case Leds::WIFISTATUS::wifi_off: wifiState = "OFF"; break;
        }

        logger.printf("\nV: %d\n", bleData.voltage);
        logger.printf("Status: %s\n", digitalRead(SWITCH_PIN) ? "ON" : "OFF");
        logger.printf("LowBattery: %s\n", lowBatteryState ? "YES" : "NO");
        logger.printf("WakeReason: %s\n", timerWakeUp ? "TIMER" : "BOOT");
        logger.printf("BLE: %s\n", btState);
        logger.printf("WiFi: %s\n", wifiState);
    }
}

void manageSensors(void * pvParameters) {
    voltage.begin();
    for(;;) {
        voltage.reading((analogRead(ADC_PIN)*ADC_VOLT_COEFF)/4095);
        bleData.voltage = voltage.getAvg();
        delay(200); 
    }
}

void manageComms(void * pvParameters) {
    for(;;) {
        if (!isModemSleepOn) ble_update(&bleData);
        delay(2000);
    }
}

void switchOn() {
    logger.println("*ON*");
    digitalWrite(SWITCH_PIN,HIGH);
    switchStatus = true;
}

void switchOff() {
    logger.println("*OFF*");
    digitalWrite(SWITCH_PIN,LOW);
    switchStatus = false;
}

void switch_setup(bool status) {
    pinMode(SWITCH_PIN,OUTPUT);
    digitalWrite(SWITCH_PIN,status?HIGH:LOW);
    switchStatus = status;
    logger.print(status?"SWT+":"SWT-");
}

void sensors_setup() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_0db);
    logger.print("SNS+");
}

void serial_setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.flush();    
}

void setup() {
    timerWakeUp = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);

    if (!timerWakeUp) {
        ota_setup();
        wifi_setup();
        switch_setup(true);
    }
    else {
        serial_setup();
        // timer wake: do not activate Wi-Fi or OTA on wake-up
        switch_setup(false);
        isWakeUp = true;
    }
    leds.setup();
    ble_setup();
    sensors_setup();
    //logger.udpListen();
    xTaskCreate(manageSensors,"SNS",8192,NULL,1,&sensorsTask);
    xTaskCreate(manageComms,"COM",8192,NULL,1,&commTask);
    if (timerWakeUp) logger.println("TMRWUP");
    else logger.println("UP");

    wakeActiveStartedAt = millis();
}

void loop() {
    check_incoming_commands();

    if (lowBatteryState) {
        leds.btStatus = Leds::BTSTATUS::bt_off;
        leds.wifiStatus = Leds::WIFISTATUS::wifi_off;
    }

    voltage.reading((analogRead(ADC_PIN)*ADC_VOLT_COEFF)/4095);
    bleData.voltage = voltage.getAvg();

    if (bleData.voltage < LOW_BATTERY_VOLTAGE_THRESHOLD) {
        lowBatteryState = true;
        switchOff();
        leds.btStatus = Leds::BTSTATUS::bt_off;
        leds.wifiStatus = Leds::WIFISTATUS::wifi_off;
        logger.printfln("LOWBAT V=%u", bleData.voltage);
    } else {
        if (lowBatteryState && bleData.voltage >= BATTERY_RECOVERY_VOLTAGE_THRESHOLD) {
            lowBatteryState = false;
            logger.printfln("Battery recovered: %u", bleData.voltage);
        }
        if (bleData.voltage >= BATTERY_RECOVERY_VOLTAGE_THRESHOLD) {
            switchOn();
        }
    }

    const unsigned long activeWindowMs = lowBatteryState ? (LOW_BATTERY_WAKE_ACTIVE_WINDOW_SEC * 1000UL) : (WAKE_ACTIVE_WINDOW_SEC * 1000UL);
    if (activeWindowMs == 0 || millis() - wakeActiveStartedAt >= activeWindowMs) {
        logger.printfln("Battery state saved: %s", lowBatteryState ? "LOW" : "OK");
        esp_deep_sleep();
    }

    if (timerWakeUp) {
        // timer wake already handled by the common sleep check above
    }

    if (isGoToSleep) {
        //modem_sleep();
        switchOff();
        lowBatteryState = (bleData.voltage < LOW_BATTERY_VOLTAGE_THRESHOLD);
        if (lowBatteryState) {
            leds.btStatus = Leds::BTSTATUS::bt_off;
            leds.wifiStatus = Leds::WIFISTATUS::wifi_off;
        }
        esp_deep_sleep();
        isGoToSleep = false;
    }
    if (isWakeUp) {
        //modem_awake();
        if (lowBatteryState) {
            switchOff();
            leds.btStatus = Leds::BTSTATUS::bt_off;
            leds.wifiStatus = Leds::WIFISTATUS::wifi_off;
        } else if (bleData.voltage >= BATTERY_RECOVERY_VOLTAGE_THRESHOLD) {
            switchOn();
        }
        isWakeUp = false;
    }
}