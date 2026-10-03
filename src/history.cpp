#include "history.h"

#include <esp_attr.h>
#include <string.h>

namespace {
constexpr uint32_t HISTORY_MAGIC = 0x55505348UL; // "UPSH"

struct RtcHistory {
    uint32_t magic;
    uint8_t next;
    uint8_t count;
    uint16_t reserved;
    uint32_t sequence;
    VoltageHistoryRecord records[RTC_HISTORY_CAPACITY];
};

RTC_DATA_ATTR RtcHistory rtcHistory;
uint8_t exportedHistory[4 + RTC_HISTORY_PAGE_CAPACITY * sizeof(VoltageHistoryRecord)];
} // namespace

void history_begin() {
    if (rtcHistory.magic == HISTORY_MAGIC && rtcHistory.next < RTC_HISTORY_CAPACITY &&
        rtcHistory.count <= RTC_HISTORY_CAPACITY) {
        return;
    }
    memset(&rtcHistory, 0, sizeof(rtcHistory));
    rtcHistory.magic = HISTORY_MAGIC;
}

uint8_t history_count() {
    history_begin();
    return rtcHistory.count;
}

void history_clear() {
    memset(&rtcHistory, 0, sizeof(rtcHistory));
    rtcHistory.magic = HISTORY_MAGIC;
}

void history_append(uint16_t millivolts, bool lowBattery, bool loadOn, bool measurementValid,
                    uint32_t epochSeconds) {
    VoltageHistoryRecord& record = rtcHistory.records[rtcHistory.next];
    record.sequence = ++rtcHistory.sequence;
    record.epochSeconds = epochSeconds;
    record.millivolts = millivolts;
    record.flags = (lowBattery ? 0x01 : 0x00) | (loadOn ? 0x02 : 0x00) |
                   (measurementValid ? 0x00 : 0x04);
    rtcHistory.next = (rtcHistory.next + 1) % RTC_HISTORY_CAPACITY;
    if (rtcHistory.count < RTC_HISTORY_CAPACITY) ++rtcHistory.count;
}

uint8_t* history_export_page(uint8_t page, size_t& length) {
    history_begin();
    const uint8_t pageCount = (rtcHistory.count + RTC_HISTORY_PAGE_CAPACITY - 1) /
                              RTC_HISTORY_PAGE_CAPACITY;
    const uint8_t firstRecord = page * RTC_HISTORY_PAGE_CAPACITY;
    const uint8_t pageRecords = firstRecord >= rtcHistory.count
                                    ? 0
                                    : min<uint8_t>(RTC_HISTORY_PAGE_CAPACITY,
                                                   rtcHistory.count - firstRecord);
    exportedHistory[0] = 1;
    exportedHistory[1] = pageRecords;
    exportedHistory[2] = page;
    exportedHistory[3] = pageCount;
    const uint8_t start = (rtcHistory.next + RTC_HISTORY_CAPACITY - rtcHistory.count) % RTC_HISTORY_CAPACITY;
    for (uint8_t index = 0; index < pageRecords; ++index) {
        const uint8_t source = (start + firstRecord + index) % RTC_HISTORY_CAPACITY;
        memcpy(exportedHistory + 4 + index * sizeof(VoltageHistoryRecord),
               &rtcHistory.records[source], sizeof(VoltageHistoryRecord));
    }
    length = 4 + pageRecords * sizeof(VoltageHistoryRecord);
    return exportedHistory;
}
