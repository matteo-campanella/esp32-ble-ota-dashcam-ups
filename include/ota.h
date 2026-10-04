#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

void ota_setup();
// Attempts UTC synchronization through NTP. It requires an established Wi-Fi
// connection and is independent from the OTA update check.
bool ota_sync_time_ntp();

#define hostString "bleApp"
// Set to 0 to keep OTA Wi-Fi updates but disable NTP clock synchronization.
#define OTA_NTP_TIME_SYNC_ENABLED 1
#define OTA_NTP_SYNC_TIMEOUT_MS 8000UL
#define OTA_NTP_PRIMARY_SERVER "pool.ntp.org"
#define OTA_NTP_FALLBACK_SERVER "time.google.com"
