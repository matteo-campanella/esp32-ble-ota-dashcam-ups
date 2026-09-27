#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Arduino.h>

#include "ble.h"
#include "configuration.h"
#include "history.h"
#include "logging.h"
#include <string>

namespace {
constexpr uint8_t COMMAND_QUEUE_DEPTH = 40;
constexpr size_t COMMAND_MAX_LENGTH = 24;
char commandQueue[COMMAND_QUEUE_DEPTH][COMMAND_MAX_LENGTH] = {};
volatile uint8_t commandHead = 0;
volatile uint8_t commandTail = 0;
volatile uint8_t commandCount = 0;
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

void enqueueCommand(const std::string& command) {
  portENTER_CRITICAL(&commandMux);
  if (commandCount < COMMAND_QUEUE_DEPTH) {
    strncpy(commandQueue[commandTail], command.c_str(), COMMAND_MAX_LENGTH - 1);
    commandQueue[commandTail][COMMAND_MAX_LENGTH - 1] = '\0';
    commandTail = (commandTail + 1) % COMMAND_QUEUE_DEPTH;
    ++commandCount;
  }
  portEXIT_CRITICAL(&commandMux);
}
} // namespace

BLEServer* pServer = NULL;
BLECharacteristic* pSensCharacteristic = NULL;
BLECharacteristic* pSettingsCharacteristic = NULL;
BLECharacteristic* pHistoryCharacteristic = NULL;
BLECharacteristic* pHistoryPage2Characteristic = NULL;
BLECharacteristic *pTxCharacteristic, *pRxCharacteristic;
BLEAdvertising* pAdvertising = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint32_t value = 0;
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
#define HISTORY_CHARACTERISTIC_UUID  "e6aa2d53-4ed4-43a6-a799-18dbf6a6d3da"
#define HISTORY_PAGE_2_CHARACTERISTIC_UUID "8e64f238-2ffc-4870-bd31-3358f3b5c82d"


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
    void onRead(BLECharacteristic *pCharacteristic) {
      if (pCharacteristic == pSettingsCharacteristic) {
        const String snapshot = configuration_export();
        pCharacteristic->setValue(snapshot.c_str());
      } else if (pCharacteristic == pHistoryCharacteristic) {
        size_t length = 0;
        uint8_t* snapshot = history_export_page(0, length);
        pCharacteristic->setValue(snapshot, length);
      } else if (pCharacteristic == pHistoryPage2Characteristic) {
        size_t length = 0;
        uint8_t* snapshot = history_export_page(1, length);
        pCharacteristic->setValue(snapshot, length);
      }
    }

    void onWrite(BLECharacteristic *pCharacteristic) {
      if (pCharacteristic == pRxCharacteristic || pCharacteristic == pSettingsCharacteristic) {
        enqueueCommand(pCharacteristic->getValue());
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
  pSettingsCharacteristic->setCallbacks(new MyCallbacks());
  pSettingsCharacteristic->setValue(configuration_export().c_str());

  pHistoryCharacteristic = pGpsService->createCharacteristic(
                      HISTORY_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ
                    );
  pHistoryCharacteristic->setCallbacks(new MyCallbacks());

  pHistoryPage2Characteristic = pGpsService->createCharacteristic(
                      HISTORY_PAGE_2_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ
                    );
  pHistoryPage2Characteristic->setCallbacks(new MyCallbacks());

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
  char command[COMMAND_MAX_LENGTH] = {};
  portENTER_CRITICAL(&commandMux);
  if (commandCount > 0) {
    strncpy(command, commandQueue[commandHead], COMMAND_MAX_LENGTH - 1);
    commandHead = (commandHead + 1) % COMMAND_QUEUE_DEPTH;
    --commandCount;
  }
  portEXIT_CRITICAL(&commandMux);
  return String(command);
}

bool ble_is_connected() {
  return deviceConnected;
}

void ble_advertise_status(uint16_t voltage, uint16_t externalSupplyVoltage,
                          bool switchOn, bool lowBattery, bool wifiConnected) {
  if (pAdvertising == NULL) return;

  // Legacy BLE advertising is limited to 31 bytes. Keep the positional payload
  // compact enough to carry both voltage metrics:
  // FF FF <battery mV>;<switch>;<low battery>;<BLE>;<Wi-Fi>;<external mV>
  // FF FF is the Bluetooth SIG test/internal company identifier; do not use
  // it for a commercial product without replacing it with an assigned ID.
  char status[48];
  snprintf(status, sizeof(status), "\xFF\xFF%u;%u;%u;%u;%u;%u",
           voltage,
           switchOn ? 1 : 0,
           lowBattery ? 1 : 0,
           deviceConnected ? 1 : 0,
           wifiConnected ? 1 : 0,
           externalSupplyVoltage);

  BLEAdvertisementData advertisementData;
  advertisementData.setFlags(0x06);
  advertisementData.setManufacturerData(std::string(status, strlen(status)));
  pAdvertising->setAdvertisementData(advertisementData);
}

void ble_update(BLEData *data) {
  if (deviceConnected && pSensCharacteristic != NULL) {
    snprintf(outBuffer, sizeof(outBuffer), "V=%u;E=%u", data->voltage,
             data->externalSupplyVoltage);
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
  portENTER_CRITICAL(&commandMux);
  commandHead = commandTail = commandCount = 0;
  portEXIT_CRITICAL(&commandMux);
  pServer = NULL;
  pAdvertising = NULL;
  pSensCharacteristic = NULL;
  pSettingsCharacteristic = NULL;
  pHistoryCharacteristic = NULL;
  pHistoryPage2Characteristic = NULL;
  pTxCharacteristic = NULL;
  pRxCharacteristic = NULL;
  BLEDevice::deinit();
}
