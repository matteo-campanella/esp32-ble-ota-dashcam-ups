#pragma once

#include <Arduino.h>

// Retained across deep-sleep resets, but intentionally lost on full power loss.
// 100 packed records occupy about 1.1 KiB of RTC slow memory.
constexpr uint8_t RTC_HISTORY_CAPACITY = 100;
// A BLE GATT characteristic value is limited to 512 bytes. A page of 45
// records occupies 499 bytes including its four-byte header; three pages hold
// all 100 retained records.
constexpr uint8_t RTC_HISTORY_PAGE_CAPACITY = 45;

struct __attribute__((packed)) VoltageHistoryRecord {
    uint32_t sequence;
    uint32_t epochSeconds; // zero until a phone has synchronized time
    uint16_t millivolts;
    uint8_t flags; // bit 0: low battery, bit 1: load on, bit 2: invalid ADC result
};

static_assert(sizeof(VoltageHistoryRecord) == 11, "History record must stay compact.");

void history_begin();
// Number of records currently retained in RTC slow memory.
uint8_t history_count();
// Discards retained records. Used when the phone establishes the first valid
// clock, so later history always has a real timestamp.
void history_clear();
void history_append(uint16_t millivolts, bool lowBattery, bool loadOn, bool measurementValid,
                    uint32_t epochSeconds);
// Binary chronological page: version, page count, page index, page total, then records.
uint8_t* history_export_page(uint8_t page, size_t& length);
