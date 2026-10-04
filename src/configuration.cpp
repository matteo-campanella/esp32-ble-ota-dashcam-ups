#include "configuration.h"
#include "history.h"

#include <Preferences.h>
#include <esp_attr.h>
#include <string.h>
#include <time.h>

namespace {
constexpr char CONFIG_NAMESPACE[] = "upsconfig";
constexpr char INTERVALS_KEY[] = "intervals";
constexpr uint32_t RTC_CLOCK_MAGIC = 0x55505343UL; // "UPSC"

// Do not give this RTC object a C++ initializer. The ESP32 startup code can
// reapply initialized RTC data on reset; an uninitialized retained block plus
// a magic value preserves state across timer deep-sleep wakes.
struct RtcClockState {
    uint32_t magic;
    uint32_t epochAtBoot;
    uint32_t previousSleepSeconds;
    bool known;
};
RTC_DATA_ATTR RtcClockState rtcClock;
unsigned long runtimeBootMillis = 0;

void saveConfiguration() {
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return;
    preferences.putUShort("veryLow", deviceConfiguration.veryLowBatteryMillivolts);
    preferences.putUShort("low", deviceConfiguration.lowBatteryMillivolts);
    preferences.putUShort("recovery", deviceConfiguration.recoveryMillivolts);
    preferences.putUShort("extHigh", deviceConfiguration.externalSupplyHighMillivolts);
    preferences.putUChar("override", static_cast<uint8_t>(deviceConfiguration.overrideMode));
    preferences.putBool("calendar", deviceConfiguration.calendarEnabled);
    preferences.putShort("utcOffset", deviceConfiguration.utcOffsetMinutes);
    preferences.putBytes(INTERVALS_KEY, deviceConfiguration.intervals,
                         sizeof(deviceConfiguration.intervals));
    preferences.end();
}

bool splitNumbers(const String& command, int32_t* values, uint8_t expected) {
    uint8_t found = 0;
    int start = 0;
    while (start <= command.length() && found < expected) {
        const int comma = command.indexOf(',', start);
        const String token = command.substring(start, comma < 0 ? command.length() : comma);
        if (token.length() == 0) return false;
        values[found++] = token.toInt();
        if (comma < 0) break;
        start = comma + 1;
    }
    return found == expected && command.indexOf(',', start) < 0;
}

uint8_t mondayFirstWeekday(const tm& localTime) {
    // tm_wday is Sunday 0 through Saturday 6; the app sends Monday 0.
    return static_cast<uint8_t>((localTime.tm_wday + 6) % WEEK_DAYS);
}
} // namespace

DeviceConfiguration deviceConfiguration;

void configuration_begin(bool timerWake) {
    Preferences preferences;
    if (preferences.begin(CONFIG_NAMESPACE, true)) {
        deviceConfiguration.veryLowBatteryMillivolts = preferences.getUShort("veryLow", 3000);
        deviceConfiguration.lowBatteryMillivolts = preferences.getUShort("low", 3300);
        deviceConfiguration.recoveryMillivolts = preferences.getUShort("recovery", 3600);
        deviceConfiguration.externalSupplyHighMillivolts = preferences.getUShort("extHigh", 11000);
        const uint8_t storedOverride = preferences.getUChar("override", 0);
        deviceConfiguration.overrideMode = storedOverride <= static_cast<uint8_t>(LoadOverride::ForceOff)
                                               ? static_cast<LoadOverride>(storedOverride)
                                               : LoadOverride::Calendar;
        deviceConfiguration.calendarEnabled = preferences.getBool("calendar", false);
        deviceConfiguration.utcOffsetMinutes = preferences.getShort("utcOffset", 0);
        if (preferences.getBytesLength(INTERVALS_KEY) == sizeof(deviceConfiguration.intervals)) {
            preferences.getBytes(INTERVALS_KEY, deviceConfiguration.intervals,
                                 sizeof(deviceConfiguration.intervals));
        }
        preferences.end();
    }

    if (rtcClock.magic != RTC_CLOCK_MAGIC) {
        memset(&rtcClock, 0, sizeof(rtcClock));
        rtcClock.magic = RTC_CLOCK_MAGIC;
    }

    runtimeBootMillis = millis();
    if (timerWake && rtcClock.known) {
        rtcClock.epochAtBoot += rtcClock.previousSleepSeconds;
    } else if (!timerWake) {
        // A cold boot has no trustworthy clock, even though configuration survives in NVS.
        rtcClock.known = false;
        rtcClock.epochAtBoot = 0;
        rtcClock.previousSleepSeconds = 0;
    }
}

void configuration_prepare_sleep(uint32_t seconds) {
    if (rtcClock.known) rtcClock.epochAtBoot = configuration_now_epoch();
    rtcClock.previousSleepSeconds = seconds;
}

bool configuration_clock_is_known() { return rtcClock.known; }

uint32_t configuration_now_epoch() {
    if (!rtcClock.known) return 0;
    return rtcClock.epochAtBoot + ((millis() - runtimeBootMillis) / 1000UL);
}

bool configuration_set_time(uint32_t epochSeconds) {
    const bool firstClockSynchronization = !rtcClock.known;
    rtcClock.epochAtBoot = epochSeconds;
    runtimeBootMillis = millis();
    rtcClock.known = true;
    if (firstClockSynchronization) {
        // Earlier retained records have no trustworthy timestamp.
        history_clear();
    }
    return firstClockSynchronization;
}

bool configuration_calendar_allows_on() {
    if (!deviceConfiguration.calendarEnabled || !configuration_clock_is_known()) return true;

    const time_t localEpoch = static_cast<time_t>(configuration_now_epoch()) +
                              static_cast<time_t>(deviceConfiguration.utcOffsetMinutes) * 60;
    tm localTime{};
    gmtime_r(&localEpoch, &localTime);
    const uint8_t day = mondayFirstWeekday(localTime);
    const uint16_t minute = static_cast<uint16_t>(localTime.tm_hour * 60 + localTime.tm_min);
    for (const OnInterval& interval : deviceConfiguration.intervals[day]) {
        if (interval.enabled && minute >= interval.startMinute && minute < interval.endMinute) return true;
    }
    return false;
}

bool configuration_load_is_allowed() {
    if (deviceConfiguration.overrideMode == LoadOverride::ForceOff) return false;
    if (deviceConfiguration.overrideMode == LoadOverride::ForceOn) return true;
    return configuration_calendar_allows_on();
}

uint32_t configuration_seconds_until_transition() {
    if (!deviceConfiguration.calendarEnabled || !configuration_clock_is_known() ||
        deviceConfiguration.overrideMode != LoadOverride::Calendar) {
        return UINT32_MAX;
    }

    const time_t localEpoch = static_cast<time_t>(configuration_now_epoch()) +
                              static_cast<time_t>(deviceConfiguration.utcOffsetMinutes) * 60;
    tm localTime{};
    gmtime_r(&localEpoch, &localTime);
    const uint32_t minuteOfWeek = mondayFirstWeekday(localTime) * MINUTES_PER_DAY +
                                  localTime.tm_hour * 60 + localTime.tm_min;
    const uint32_t secondsIntoMinute = localTime.tm_sec;
    uint32_t nearest = UINT32_MAX;
    for (uint8_t day = 0; day < WEEK_DAYS; ++day) {
        for (const OnInterval& interval : deviceConfiguration.intervals[day]) {
            if (!interval.enabled) continue;
            const uint16_t endpoints[] = {interval.startMinute, interval.endMinute};
            for (const uint16_t endpoint : endpoints) {
                const uint32_t eventMinute = day * MINUTES_PER_DAY + endpoint;
                uint32_t minutesUntil = (eventMinute + WEEK_DAYS * MINUTES_PER_DAY - minuteOfWeek) %
                                        (WEEK_DAYS * MINUTES_PER_DAY);
                if (minutesUntil == 0) minutesUntil = WEEK_DAYS * MINUTES_PER_DAY;
                const uint32_t secondsUntil = minutesUntil * 60U - secondsIntoMinute;
                if (secondsUntil < nearest) nearest = secondsUntil;
            }
        }
    }
    return nearest;
}

String configuration_export() {
    String value = "C3,";
    value += deviceConfiguration.veryLowBatteryMillivolts;
    value += ',';
    value += deviceConfiguration.lowBatteryMillivolts;
    value += ',';
    value += deviceConfiguration.recoveryMillivolts;
    value += ',';
    value += deviceConfiguration.externalSupplyHighMillivolts;
    value += ',';
    value += static_cast<uint8_t>(deviceConfiguration.overrideMode);
    value += ',';
    value += deviceConfiguration.calendarEnabled ? '1' : '0';
    value += ',';
    value += deviceConfiguration.utcOffsetMinutes;
    for (uint8_t day = 0; day < WEEK_DAYS; ++day) {
        for (const OnInterval& interval : deviceConfiguration.intervals[day]) {
            value += ',';
            if (interval.enabled) {
                value += interval.startMinute;
                value += ':';
                value += interval.endMinute;
            } else {
                value += "-1:-1";
            }
        }
    }
    return value;
}

bool configuration_handle_command(const String& command, String& response) {
    int32_t values[5] = {};
    if (command.startsWith("T,") && splitNumbers(command.substring(2), values, 2)) {
        if (values[0] < 1700000000L || values[1] < -840 || values[1] > 840) {
            response = "CFG ERR TIME";
        } else {
            const bool firstClockSynchronization =
                configuration_set_time(static_cast<uint32_t>(values[0]));
            deviceConfiguration.utcOffsetMinutes = static_cast<int16_t>(values[1]);
            saveConfiguration();
            if (firstClockSynchronization) {
                response = "CFG OK TIME FIRST";
            } else {
                response = "CFG OK TIME";
            }
        }
        return true;
    }

    if (command.startsWith("M,") && splitNumbers(command.substring(2), values, 1)) {
        if (values[0] < 0 || values[0] > 2) response = "CFG ERR MODE";
        else {
            deviceConfiguration.overrideMode = static_cast<LoadOverride>(values[0]);
            saveConfiguration();
            response = "CFG OK MODE";
        }
        return true;
    }

    if (command.startsWith("B,") && splitNumbers(command.substring(2), values, 2)) {
        if (values[0] < 2500 || values[0] > 5000 || values[1] < values[0] || values[1] > 5500) {
            response = "CFG ERR BAT";
        } else {
            deviceConfiguration.lowBatteryMillivolts = static_cast<uint16_t>(values[0]);
            deviceConfiguration.recoveryMillivolts = static_cast<uint16_t>(values[1]);
            saveConfiguration();
            response = "CFG OK BAT";
        }
        return true;
    }

    if (command.startsWith("V,") && splitNumbers(command.substring(2), values, 1)) {
        if (values[0] < 2000 || values[0] > deviceConfiguration.lowBatteryMillivolts) {
            response = "CFG ERR VLOW";
        } else {
            deviceConfiguration.veryLowBatteryMillivolts = static_cast<uint16_t>(values[0]);
            saveConfiguration();
            response = "CFG OK VLOW";
        }
        return true;
    }

    if (command.startsWith("X,") && splitNumbers(command.substring(2), values, 1)) {
        if (values[0] < 1000 || values[0] > 20000) {
            response = "CFG ERR EXT";
        } else {
            deviceConfiguration.externalSupplyHighMillivolts = static_cast<uint16_t>(values[0]);
            saveConfiguration();
            response = "CFG OK EXT";
        }
        return true;
    }

    if (command.startsWith("E,") && splitNumbers(command.substring(2), values, 1)) {
        if (values[0] != 0 && values[0] != 1) response = "CFG ERR CAL";
        else {
            deviceConfiguration.calendarEnabled = values[0] == 1;
            saveConfiguration();
            response = "CFG OK CAL";
        }
        return true;
    }

    if (command.startsWith("I,") && splitNumbers(command.substring(2), values, 4)) {
        const int32_t day = values[0];
        const int32_t slot = values[1];
        const int32_t start = values[2];
        const int32_t end = values[3];
        if (day < 0 || day >= WEEK_DAYS || slot < 0 || slot >= MAX_INTERVALS_PER_DAY) {
            response = "CFG ERR INT";
        } else if (start == -1 && end == -1) {
            deviceConfiguration.intervals[day][slot] = OnInterval{};
            saveConfiguration();
            response = "CFG OK INT";
        } else if (start < 0 || end > MINUTES_PER_DAY || start >= end) {
            response = "CFG ERR INT";
        } else {
            OnInterval& interval = deviceConfiguration.intervals[day][slot];
            interval.startMinute = static_cast<uint16_t>(start);
            interval.endMinute = static_cast<uint16_t>(end);
            interval.enabled = true;
            saveConfiguration();
            response = "CFG OK INT";
        }
        return true;
    }

    return false;
}
