#include <Arduino.h>
#include "logging.h"

#define RED_LED 23

class Leds{
    private:
        static TaskHandle_t greenLedTask,redLedTask;
        static unsigned int redOn,redOff,greenOff,greenOn;
    public:
        enum BLINKMODE {
            blink_off,
            blink_fast,
            blink_slow,
        };
        static BLINKMODE blinkMode;
        static void setup();
        static void setBlinkMode(BLINKMODE mode);
        static void manageRedLed(void *);
};