#ifdef TEST_HARNESS

#include <Arduino.h>
#include "network/fluidnc_client.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[TestHarness] Starting FluidNC handleIncoming test harness");

    FluidNCClient::init();

    const char* tests[] = {
        "<Idle|MPos:0.000,0.000,0.000|FS:0,0|Ov:100,100,100>",
        "<Idle|MPos:10.500,5.250,-1.234|FS:120.0,1500|WCO:0.500,0.250,1.000|Ov:90,100,100>",
        "<Run|MPos:50.000,25.000,-5.000|FS:300.0,0|SD:12.5,example.gcode>",
        "[PRB:151.000,149.000,-137.505:1]",
        "[MSG:websocket auto report interval set to 250ms]"
    };

    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i) {
        Serial.printf("[TestHarness] Feeding payload: %s\n", tests[i]);
        FluidNCClient::test_handleIncoming(tests[i]);
        delay(250);
    }

    Serial.println("[TestHarness] Completed tests. Halting loop.");
}

void loop() {
    // Do nothing - harness ran in setup
    delay(1000);
}

#endif // TEST_HARNESS
