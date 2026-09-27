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
#define DEEP_SLEEP_INTERVAL_SEC 10ULL
#define LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC 600ULL
#define CONNECTION_WINDOW_SEC 10UL
#define LOW_BATTERY_CONNECTION_WINDOW_SEC 1UL
// A connected phone is actively configuring or inspecting the device. Keep
// the radio up long enough to complete the BLE transfer, even on low battery.
#define BLE_CONNECTED_WINDOW_SEC 10UL
#define BATTERY_VALIDATION_SAMPLES 10U
#define BATTERY_VALIDATION_SAMPLE_MS 200UL
#define BATTERY_VALIDATION_TIMEOUT_MS 5000UL
#define ADC_PIN 35
// Divider wiring: battery positive -> 300k -> ADC_PIN -> 91k -> GND.
// Battery voltage is ADC voltage multiplied by (300k + 91k) / 91k = 4.297.
// A 4.20 V cell therefore produces about 0.978 V at the ADC pin.
#define BATTERY_DIVIDER_TOP_OHMS 300000UL
#define BATTERY_DIVIDER_BOTTOM_OHMS 91000UL
#define BATTERY_DIVIDER_NUMERATOR (BATTERY_DIVIDER_TOP_OHMS + BATTERY_DIVIDER_BOTTOM_OHMS)
// Calibrated external-supply scale: a measured 0.655 V ADC input corresponds
// to a measured 11.33 V supply, so supply = ADC * 17.298.
#define EXTERNAL_SUPPLY_ADC_PIN 32
#define EXTERNAL_SUPPLY_CALIBRATION_X1000 17298UL
#define SWITCH_PIN 33

