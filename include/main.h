#include <CircularBuffer.hpp>
#include <WifiUdp.h>
#include <Wire.h>
#include "logging.h"
#include "ble.h"
#include "bledata.h"
#include "wifi_credentials.h"
#include "ota.h"
#include "leds.h"
#include <movingAvg.h>

#define USE_WIFI true
#define DEEP_SLEEP_INTERVAL_SEC 30ULL
#define LOW_BATTERY_DEEP_SLEEP_INTERVAL_SEC 30ULL
#define WAKE_ACTIVE_WINDOW_SEC 10UL
#define LOW_BATTERY_WAKE_ACTIVE_WINDOW_SEC 10UL
#define LOW_BATTERY_VOLTAGE_THRESHOLD 3300U
#define BATTERY_RECOVERY_VOLTAGE_THRESHOLD 3600U
#define ADC_PIN 35
#define ADC_VOLT_COEFF 1678
#define SWITCH_PIN 33

