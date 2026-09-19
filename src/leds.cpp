#include "leds.h"

TaskHandle_t Leds::redLedTask,Leds::greenLedTask;

unsigned int Leds::redOn = 100;
unsigned int Leds::redOff = 100;

Leds::BTSTATUS Leds::btStatus;
Leds::WIFISTATUS Leds::wifiStatus;

extern Logger logger;

void Leds::manageRedLed(void * pvParameters){
  for(;;){
    if (btStatus == bt_connected && wifiStatus == wifi_connected) {
      redOn = 50;
      redOff = 50;
    }
    else if (btStatus == bt_connected) {
      redOn = 200;
      redOff = 50;
    }
    else if (wifiStatus == wifi_connected) {
      redOn = 50;
      redOff = 200;
    }
    else if (btStatus == bt_on && wifiStatus == wifi_on) {
      redOn = 100;
      redOff = 100;
    }
    else if (btStatus == bt_on) {
      redOn = 300;
      redOff = 300;
    }
    else if (wifiStatus == wifi_on) {
      redOn = 400;
      redOff = 100;
    }
    else {
      redOn = 1;
      redOff = 999;
    }

    digitalWrite(RED_LED, HIGH);
    delay(redOn);
    digitalWrite(RED_LED, LOW);
    delay(redOff);
  }
}
  
void Leds::setup() {
    btStatus = bt_off;
    wifiStatus = wifi_off;
    pinMode(RED_LED,OUTPUT);
    xTaskCreate(Leds::manageRedLed,"redLed",1024,NULL,10,&redLedTask); 
    logger.print("LED+"); 
}