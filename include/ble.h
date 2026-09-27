#include <string>
#include "bledata.h"

void ble_setup();
void ble_update(BLEData *);
void ble_uart_send(const char *);
String ble_uart_receive();
bool ble_is_connected();
// Broadcasts the compact ordered status fields documented in src/ble.cpp.
void ble_advertise_status(uint16_t voltage, uint16_t externalSupplyVoltage,
                          bool switchOn, bool lowBattery, bool wifiConnected);
void ble_stop();
