#pragma once

#include <Arduino.h>

constexpr uint8_t WEEK_DAYS = 7;
constexpr uint8_t MAX_INTERVALS_PER_DAY = 4;
constexpr uint16_t MINUTES_PER_DAY = 24U * 60U;

enum class LoadOverride : uint8_t {
    Calendar = 0,
    ForceOn = 1,
    ForceOff = 2,
};

struct OnInterval {
    uint16_t startMinute = 0;
    uint16_t endMinute = 0;
    bool enabled = false;
};

struct DeviceConfiguration {
    uint16_t lowBatteryMillivolts = 3300;
    uint16_t recoveryMillivolts = 3600;
    LoadOverride overrideMode = LoadOverride::Calendar;
    bool calendarEnabled = false; // false is the safe, backwards-compatible always-on default.
    int16_t utcOffsetMinutes = 0;
    OnInterval intervals[WEEK_DAYS][MAX_INTERVALS_PER_DAY]; // Monday is day 0.
};

extern DeviceConfiguration deviceConfiguration;

// Loads persistent configuration and restores the RTC-retained clock estimate.
void configuration_begin(bool timerWake);
// Stores the sleep duration so a timer wake can advance the retained clock.
void configuration_prepare_sleep(uint32_t seconds);

bool configuration_clock_is_known();
uint32_t configuration_now_epoch();
bool configuration_calendar_allows_on();
bool configuration_load_is_allowed();
uint32_t configuration_seconds_until_transition();

// Complete, authoritative snapshot for the BLE Settings characteristic.
// C1,<low>,<recovery>,<mode>,<calendar>,<UTC offset>,<start:end> x 28
String configuration_export();

// Handles compact BLE/UART configuration commands. Returns true if the command
// belongs to this protocol and puts a concise reply in response.
// T,<epoch>,<UTC offset minutes>  synchronize time
// M,<0|1|2>                       calendar / force on / force off
// B,<low mV>,<recovery mV>        battery thresholds
// E,<0|1>                         calendar disabled / enabled
// I,<day>,<slot>,<start>,<end>    weekly on interval; -1,-1 disables a slot
bool configuration_handle_command(const String& command, String& response);
