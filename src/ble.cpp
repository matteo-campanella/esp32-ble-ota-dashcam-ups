#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Arduino.h>

#include "ble.h"
#include "logging.h"
#include <string>

BLEServer* pServer = NULL;
BLECharacteristic* pSensCharacteristic = NULL;
BLECharacteristic* pSettingsCharacteristic = NULL;
BLECharacteristic *pTxCharacteristic, *pRxCharacteristic;
BLEAdvertising* pAdvertising = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint32_t value = 0;
String inCommand;
String tmp;
char outBuffer[80];

extern Logger logger;

#define BLE_DEVICE_NAME "bleUPS"
#define UART_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define UART_CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define UPS_SERVICE_UUID          "d96011fc-8ab0-42d9-93bb-ae202331297a"
#define SENSORS_CHARACTERISTIC_UUID  "7bfb13b9-917f-44e6-9eac-7739088a0783"
#define SETTINGS_CHARACTERISTIC_UUID "235fefc9-58fd-4f84-977a-9a72ae348007"


class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      BLEDevice::startAdvertising();
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      if (pCharacteristic == pRxCharacteristic) {
        inCommand = pCharacteristic->getValue().c_str();
      }
      else if (pCharacteristic == pSettingsCharacteristic) {
        logger.print("settings write");
      }
    }
};

void ble_setup() {
  if (pServer != NULL) return;
  // Create the BLE Device
  BLEDevice::init(BLE_DEVICE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  //UART Service
  BLEService *pUARTService = pServer->createService(UART_SERVICE_UUID);
  pTxCharacteristic = pUARTService->createCharacteristic(
										UART_CHARACTERISTIC_UUID_TX,
										BLECharacteristic::PROPERTY_NOTIFY
									);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pRxCharacteristic = pUARTService->createCharacteristic(
											UART_CHARACTERISTIC_UUID_RX,
											BLECharacteristic::PROPERTY_WRITE
										);
  pRxCharacteristic->setCallbacks(new MyCallbacks());     
  pUARTService->start();

  //Motion Service
  BLEService *pGpsService = pServer->createService(UPS_SERVICE_UUID);
  pSensCharacteristic = pGpsService->createCharacteristic(
                      SENSORS_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pSensCharacteristic->addDescriptor(new BLE2902());

  pSettingsCharacteristic = pGpsService->createCharacteristic(
                      SETTINGS_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE
                    );
  //pSettingsCharacteristic->addDescriptor(new BLE2902());  

  pGpsService->start();
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(UPS_SERVICE_UUID);
  pAdvertising->addServiceUUID(UART_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  logger.print("BLE+");
}

void ble_uart_send(const char *message) {
    if (deviceConnected && pTxCharacteristic != NULL) {
      pTxCharacteristic->setValue((uint8_t*)message,strlen(message));
      pTxCharacteristic->notify();
		  delay(10); // bluetooth stack will go into congestion, if too many packets are sent
	  }
}

String ble_uart_receive() {
  tmp = "";
  if (inCommand.length()>0) {
    tmp = inCommand;
    inCommand = "";
  }
  return tmp;
}

bool ble_is_connected() {
  return deviceConnected;
}

void ble_advertise_status(uint16_t voltage, bool switchOn, bool lowBattery, bool wifiConnected) {
  if (pAdvertising == NULL) return;

  // Legacy BLE advertising is limited to 31 bytes.  This manufacturer-data
  // payload carries all dump fields in an easy-to-decode form:
  // FF FF V=<mV>;S=<0|1>;L=<0|1>;B=<0|1>;W=<0|1>
  // FF FF is the Bluetooth SIG test/internal company identifier; do not use
  // it for a commercial product without replacing it with an assigned ID.
  char status[48];
  snprintf(status, sizeof(status), "\xFF\xFFV=%u;S=%u;L=%u;B=%u;W=%u",
           voltage,
           switchOn ? 1 : 0,
           lowBattery ? 1 : 0,
           deviceConnected ? 1 : 0,
           wifiConnected ? 1 : 0);

  BLEAdvertisementData advertisementData;
  advertisementData.setFlags(0x06);
  advertisementData.setManufacturerData(std::string(status, strlen(status)));
  pAdvertising->setAdvertisementData(advertisementData);
}

void ble_update(BLEData *data) {
  if (deviceConnected && pSensCharacteristic != NULL) {
    snprintf(outBuffer, sizeof(outBuffer), "%.2f", data->voltage / 100.0);
    pSensCharacteristic->setValue(outBuffer);
    pSensCharacteristic->notify();
  }
  // disconnecting
  if (pServer != NULL && !deviceConnected && oldDeviceConnected) {
      delay(500); // give the bluetooth stack the chance to get things ready
      pServer->startAdvertising(); // restart advertising
      oldDeviceConnected = deviceConnected;
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = deviceConnected;
  }
}

void ble_stop() {
  if (pServer == NULL) return;
  deviceConnected = false;
  oldDeviceConnected = false;
  pServer = NULL;
  pAdvertising = NULL;
  pSensCharacteristic = NULL;
  pSettingsCharacteristic = NULL;
  pTxCharacteristic = NULL;
  pRxCharacteristic = NULL;
  BLEDevice::deinit();
}
