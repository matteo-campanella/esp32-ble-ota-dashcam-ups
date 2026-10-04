#include <CircularBuffer.hpp>
#include <WifiUdp.h>
#include <Wire.h>
#include "logging.h"
#include "ble.h"
#include "bledata.h"
#include "wifi_credentials.h"
#include "ota.h"
#include "leds.h"
#include "configuration.h"
#include "history.h"
#define USE_WIFI false
// awake durations in seconds
#define EXTERNAL_SUPPLY_CONNECTION_WINDOW_SEC 30UL
#define CONNECTION_WINDOW_SEC 10UL
#define LOW_BATTERY_CONNECTION_WINDOW_SEC 10UL
#define VERY_LOW_BATTERY_CONNECTION_WINDOW_SEC 8UL
// sleep durations in seconds
#define EXTERNAL_SUPPLY_DEEP_SLEEP_INTERVAL_SEC 30ULL
#define DEEP_SLEEP_INTERVAL_SEC 90ULL
#define LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC 900ULL
#define VERY_LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC 3600ULL
// The external-supply high trigger is a persisted BLE/mobile setting.
// A connected phone is actively configuring or inspecting the device. Keep
// the radio up long enough to complete the BLE transfer, even on low battery.
#define BLE_CONNECTED_WINDOW_SEC 30UL
//
#define BATTERY_VALIDATION_SAMPLES 10U
#define BATTERY_VALIDATION_SAMPLE_MS 200UL
#define BATTERY_VALIDATION_TIMEOUT_MS 5000UL
//
#define ADC_PIN 35
#define BATTERY_CALIBRATION_X1000 4297UL
#define EXTERNAL_SUPPLY_ADC_PIN 32
#define EXTERNAL_SUPPLY_CALIBRATION_X1000 17298UL
#define SWITCH_PIN 33

