#include "leds.h"

TaskHandle_t Leds::redLedTask,Leds::greenLedTask;

unsigned int Leds::redOn = 100;
unsigned int Leds::redOff = 100;

Leds::BLINKMODE Leds::blinkMode = Leds::blink_off;

extern Logger logger;

void Leds::setBlinkMode(BLINKMODE mode) {
    if (blinkMode != mode) {
        blinkMode = mode;
    }
}

void Leds::manageRedLed(void * pvParameters){
  for(;;){
    switch (blinkMode) {
        case blink_fast:
            digitalWrite(RED_LED, (millis() % 200UL) < 100UL ? HIGH : LOW);
            break;
        case blink_slow:
            digitalWrite(RED_LED, (millis() % 1000UL) < 500UL ? HIGH : LOW);
            break;
        case blink_off:
        default:
            digitalWrite(RED_LED, LOW);
            break;
    }
    delay(50);
  }
}
  
void Leds::setup() {
    blinkMode = blink_off;
    pinMode(RED_LED,OUTPUT);
    xTaskCreate(Leds::manageRedLed,"redLed",1024,NULL,10,&redLedTask); 
    logger.print("LED+"); 
}
