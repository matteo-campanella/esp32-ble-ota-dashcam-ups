#include <string>
#include "bledata.h"

void ble_setup();
void ble_update(BLEData *);
void ble_uart_send(const char *);
String ble_uart_receive();
bool ble_is_connected();
// Broadcasts the same status fields exposed by the dump command. The compact
// wire format is documented in src/ble.cpp.
void ble_advertise_status(uint16_t voltage, bool switchOn, bool lowBattery, bool wifiConnected);
void ble_stop();
