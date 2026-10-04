#include "ota.h"
#include "ota_credentials.h"
#include "configuration.h"

#include <time.h>

WiFiClient ota_client;

bool ota_sync_time_ntp() {
#if OTA_NTP_TIME_SYNC_ENABLED
    // NTP supplies UTC. The calendar's local-time offset remains the value
    // configured by the phone and stored in NVS.
    configTime(0, 0, OTA_NTP_PRIMARY_SERVER, OTA_NTP_FALLBACK_SERVER);
    const unsigned long startedAt = millis();
    time_t epoch = 0;
    while (millis() - startedAt < OTA_NTP_SYNC_TIMEOUT_MS) {
        epoch = time(nullptr);
        if (epoch >= 1700000000) {
            const bool firstSync = configuration_set_time(static_cast<uint32_t>(epoch));
            Serial.printf("NTP time synchronized: %lu%s\n", static_cast<unsigned long>(epoch),
                          firstSync ? " (first sync)" : "");
            return true;
        }
        delay(200);
    }
    Serial.println("NTP time synchronization failed; awaiting phone time sync.");
    return false;
#else
    return false;
#endif
}

bool initNetwork() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(OTA_SSID, OTA_PASS);
    int tries = 50;
    while (WiFi.status() != WL_CONNECTED && tries > 0) {
        delay(250);
        tries--;
    }
    if (tries == 0) {
        Serial.println("Failed to connect to WiFi for OTA!");
        return false;
    }
    Serial.print("IP: ");
    Serial.println(WiFi.localIP().toString());
    return true;
}

void initMDns() {
    if (!MDNS.begin(hostString)) {
      Serial.println("Error setting up MDNS responder!");
    }
    Serial.println("mDNS responder started");
}

void checkUpdates() {
    Serial.println("Sending mDNS query");
    int n = MDNS.queryService("espupdate", "tcp");
    Serial.println("mDNS query done");
    if (n == 0) {
      Serial.println("no update services found");
      return;
    }
    else {
      Serial.print(n);
      Serial.println(" service(s) found");
      for (int i = 0; i < n; ++i) {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(MDNS.hostname(i));
        Serial.print(" (");
        Serial.print(MDNS.IP(i).toString());
        Serial.print(":");
        Serial.print(MDNS.port(i));
        Serial.println(")");
      }
    }
    Serial.println();
    String url = "http://" + MDNS.IP(0).toString() + ":" + MDNS.port(0) + "/espupdate?n=" SW_NAME "&v=" SW_VERSION;
    Serial.println("Checking firmware update from " + url);
    t_httpUpdate_return ret = httpUpdate.update(ota_client, url);
    switch (ret) {
      case HTTP_UPDATE_FAILED: Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str()); break;
      case HTTP_UPDATE_NO_UPDATES: Serial.println("HTTP_UPDATE_NO_UPDATES"); break;
      case HTTP_UPDATE_OK: Serial.println("HTTP_UPDATE_OK"); break;
    }
}

void ota_setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.flush();

  if (initNetwork()) {
    ota_sync_time_ntp();
    initMDns();
    checkUpdates();
  }
}

