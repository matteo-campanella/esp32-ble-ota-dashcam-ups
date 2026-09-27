#include "configuration.h"

#include <Preferences.h>
#include <esp_attr.h>
#include <time.h>

namespace {
constexpr char CONFIG_NAMESPACE[] = "upsconfig";
constexpr char INTERVALS_KEY[] = "intervals";

RTC_DATA_ATTR bool rtcClockKnown = false;
RTC_DATA_ATTR uint32_t rtcEpochAtBoot = 0;
RTC_DATA_ATTR uint32_t rtcPreviousSleepSeconds = 0;
unsigned long runtimeBootMillis = 0;

void saveConfiguration() {
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return;
    preferences.putUShort("low", deviceConfiguration.lowBatteryMillivolts);
    preferences.putUShort("recovery", deviceConfiguration.recoveryMillivolts);
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
        deviceConfiguration.lowBatteryMillivolts = preferences.getUShort("low", 3300);
        deviceConfiguration.recoveryMillivolts = preferences.getUShort("recovery", 3600);
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

    runtimeBootMillis = millis();
    if (timerWake && rtcClockKnown) {
        rtcEpochAtBoot += rtcPreviousSleepSeconds;
    } else if (!timerWake) {
        // A cold boot has no trustworthy clock, even though configuration survives in NVS.
        rtcClockKnown = false;
        rtcEpochAtBoot = 0;
    }
}

void configuration_prepare_sleep(uint32_t seconds) {
    if (rtcClockKnown) rtcEpochAtBoot = configuration_now_epoch();
    rtcPreviousSleepSeconds = seconds;
}

bool configuration_clock_is_known() { return rtcClockKnown; }

uint32_t configuration_now_epoch() {
    if (!rtcClockKnown) return 0;
    return rtcEpochAtBoot + ((millis() - runtimeBootMillis) / 1000UL);
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
    String value = "C1,";
    value += deviceConfiguration.lowBatteryMillivolts;
    value += ',';
    value += deviceConfiguration.recoveryMillivolts;
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
            rtcEpochAtBoot = static_cast<uint32_t>(values[0]);
            runtimeBootMillis = millis();
            rtcClockKnown = true;
            deviceConfiguration.utcOffsetMinutes = static_cast<int16_t>(values[1]);
            saveConfiguration();
            response = "CFG OK TIME";
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
