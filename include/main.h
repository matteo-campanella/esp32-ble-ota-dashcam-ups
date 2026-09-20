#include <CircularBuffer.hpp>
#include <WifiUdp.h>
#include <Wire.h>
#include "logging.h"
#include "ble.h"
#include "bledata.h"
#include "wifi_credentials.h"
#include "ota.h"
#include "leds.h"
#define USE_WIFI false
#define DEEP_SLEEP_INTERVAL_SEC 10ULL
#define LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC 10ULL
#define CONNECTION_WINDOW_SEC 10UL
#define LOW_BATTERY_CONNECTION_WINDOW_SEC 10UL
#define BATTERY_VALIDATION_SAMPLES 10U
#define BATTERY_VALIDATION_SAMPLE_MS 200UL
#define BATTERY_VALIDATION_TIMEOUT_MS 5000UL
#define LOW_BATTERY_VOLTAGE_THRESHOLD 3300U
#define BATTERY_RECOVERY_VOLTAGE_THRESHOLD 3600U
#define ADC_PIN 35
// Divider wiring: battery positive -> 300k -> ADC_PIN -> 91k -> GND.
// Battery voltage is ADC voltage multiplied by (300k + 91k) / 91k = 4.297.
// A 4.20 V cell therefore produces about 0.978 V at the ADC pin.
#define BATTERY_DIVIDER_TOP_OHMS 300000UL
#define BATTERY_DIVIDER_BOTTOM_OHMS 91000UL
#define BATTERY_DIVIDER_NUMERATOR (BATTERY_DIVIDER_TOP_OHMS + BATTERY_DIVIDER_BOTTOM_OHMS)
#define SWITCH_PIN 33

