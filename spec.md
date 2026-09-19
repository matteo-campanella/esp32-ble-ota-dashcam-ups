# ESP32 Timer Wake / Low-Battery Deep Sleep Specification

## 1. Purpose

This firmware is designed to operate in a low-power cycle based on periodic deep-sleep wakeups, rather than wake-on-external-interrupt behavior.

The device shall:
- remain in deep sleep for a fixed interval `N` seconds,
- wake up periodically using the ESP32 RTC timer,
- stay awake for a short active window `N_active` seconds,
- measure the system voltage using the existing ADC-based voltage sensor,
- detect a low-battery condition using a configurable threshold `Vt`,
- keep the load switched off while voltage remains below `Vt`,
- re-enable the load only after voltage rises above a higher recovery threshold `Vrec`,
- save the low-battery state before returning to deep sleep,
- and avoid any Wi‑Fi activity during normal timer wake unless an OTA maintenance flow is explicitly started.

## 2. Wakeup model

### 2.1 Sleep source
The system shall use the ESP32 internal timer as the only wake source for deep sleep.

- No external interrupt wake source shall be used for normal operation.
- The firmware shall not depend on `GPIO_NUM_34` or similar ext0 wake logic for standard wakeups.
- The deep sleep routine shall call `esp_sleep_enable_timer_wakeup()` with a value derived from the configured interval in seconds.

### 2.2 Timer interval
The sleep interval shall be configured as a single constant in seconds:

- `DEEP_SLEEP_INTERVAL_SEC = N`

This value defines how long the device remains in deep sleep before waking up again.

### 2.3 Active window after wake
After each timer wake, the device shall remain active for a configurable active window `N_active` seconds.

- The active window is a timed runtime period after wake.
- During this time, the system may sample sensors and decide whether to remain awake, transmit status, or prepare for sleep.
- Once the action window completes, the device shall return to deep sleep.

### 2.4 Wake reason awareness
The firmware must be able to distinguish between:
- initial power-up / cold boot,
- timer wake after deep sleep.

This is required so that the firmware can avoid running OTA/Wi‑Fi startup logic on timer wakes.

The firmware shall treat a timer wake as a normal scheduled wake and must not execute OTA logic unless a separate explicit mode is configured.

## 3. Runtime behavior

### 3.1 Power-on / cold boot
On first startup, the device may perform normal boot tasks such as:
- sensor setup,
- LED setup,
- BLE setup,
- initial configuration,
- OTA/Wi‑Fi setup if enabled for maintenance.

This mode is not considered the periodic timer wake behavior.

### 3.2 Timer wake behavior
When the ESP32 wakes due to the RTC timer:
- the firmware shall identify the wake reason as `ESP_SLEEP_WAKEUP_TIMER`,
- it shall not perform OTA update checks,
- it shall not bring up Wi‑Fi for OTA,
- it shall skip any Wi‑Fi connection logic unless a dedicated override flag is set,
- it shall continue with the short active task window.

## 4. Voltage sensing and low-battery logic

### 4.1 Existing sensor
The device already includes a voltage sampling path using the ADC and `movingAvg` logic.

The firmware shall continue using the existing ADC voltage reading implementation during the active window.

### 4.2 Battery thresholds
Two configurable thresholds shall be defined as system parameters in volts or ADC-equivalent units depending on the calibration currently used by the project:

- `Vt`: low-battery cutoff threshold
- `Vrec`: battery recovery threshold, higher than `Vt`

Behavior:
- If measured voltage < `Vt`, the device shall enter a low-battery condition and the load shall be switched off.
- The load shall remain off while the voltage stays below `Vrec`.
- Once measured voltage >= `Vrec`, the load may be switched back on.
- The low-battery flag shall be latched in persistent / retained state before entering deep sleep again.

### 4.3 Persistent low-battery state
The low-battery state shall be saved before the device goes back into deep sleep.

This may be done using one of the following approaches:
- RTC memory retention (`RTC_DATA_ATTR` / `RTC_SLOW_ATTR`), or
- a retained variable suitable for ESP32 deep-sleep resume state, or
- a nonvolatile storage mechanism if the design later requires it.

The requirement is that the state survives sleep and can be read on the next wake cycle or after restart.

### 4.4 On low battery and recovery
If the measured voltage falls below `Vt`:
- set the low-battery flag,
- switch the load off immediately,
- record the event in logs or retained state,
- avoid any OTA / Wi‑Fi maintenance activity during the timer wake cycle,
- proceed to the next deep-sleep cycle without attempting network recovery.

If the measured voltage later rises to or above `Vrec`:
- clear the low-battery state,
- allow the load to be switched back on,
- continue the active window normally.

This ensures battery preservation remains the priority while allowing automatic recovery once the system is healthy again.

## 5. Required firmware logic changes

### 5.1 Sleep function
The sleep function shall:
- disable any non-timer wake source,
- enable timer wake to resume after `N` seconds,
- log the transition to deep sleep,
- enter deep sleep.

Pseudo-logic:

```cpp
void esp_deep_sleep() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_INTERVAL_SEC * 1000000ULL);
    logger.println("Entering Deep Sleep...");
    delay(1000);
    esp_deep_sleep_start();
}
```

### 5.2 Wake detection
The firmware shall detect the timer wake using:

```cpp
esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER
```

and not use `ESP_SLEEP_WAKEUP_EXT0` as the normal wake reason.

### 5.3 Active window
The active period after wake shall be controlled by a dedicated timer or a simple loop timeout.

The firmware shall:
- wake from deep sleep,
- run the measurement path,
- evaluate battery voltage,
- set the battery state if needed,
- then sleep again after the active window expires.

### 5.4 No OTA on timer wake
The firmware shall not enable or connect Wi‑Fi just because a timer wake occurred.

The OTA path is explicitly disallowed during normal timer wake cycles.

## 6. Configuration values
The project shall define all behavioral values in one place for easier tuning.

Required settings:
- `DEEP_SLEEP_INTERVAL_SEC` = wake every `N` seconds
- `WAKE_ACTIVE_WINDOW_SEC` = active window after wake, e.g. `N_active`
- `LOW_BATTERY_VOLTAGE_THRESHOLD` = low-battery cutoff `Vt`
- `BATTERY_RECOVERY_VOLTAGE_THRESHOLD` = recovery threshold `Vrec`
- `USE_WIFI` = `true` only for OTA maintenance, otherwise `false` for the normal timer wake path
- optional `ALLOW_OTA_ON_TIMER_WAKE` = `false` by default

## 7. Functional requirements summary

The firmware shall satisfy all of the following:

1. The device sleeps for `N` seconds between wakeups.
2. The device wakes via the internal RTC timer only.
3. After each timer wake, a short active window `N_active` seconds is entered.
4. During the active window, the voltage sensor is checked.
5. If measured voltage is below `Vt`, the load is switched off and the low-battery state is set and retained.
6. The low-battery state is saved before the next sleep.
7. The load remains off until the voltage is above `Vrec`.
8. Wi‑Fi is reserved for OTA maintenance and is not activated during a normal timer wake.
9. The firmware can tell the difference between cold boot and timer wake.
10. The sleep cycle repeats automatically with no external interrupt dependency.

## 8. Non-functional constraints

- Prioritize current conservation.
- Avoid network activity during the timer-driven wake cycle.
- Minimize CPU time during deep sleep and wake windows.
- Keep configuration centralized for easy adjustment of `N`, `N_active`, and `Vt`.
- Ensure all deep-sleep behavior is deterministic and repeatable.

## 9. Acceptance criteria

The feature is complete when:
- the project no longer depends on ext0 wake behavior for regular operation,
- the firmware wakes every `N` seconds using the timer,
- the active window runs after each wake,
- measured voltage is evaluated against both `Vt` and `Vrec`,
- the load is switched off below `Vt` and re-enabled above `Vrec`,
- low-battery state is persisted before sleep,
- OTA/Wi‑Fi is not triggered on timer wakes by default and is explicitly reserved for OTA maintenance only.
