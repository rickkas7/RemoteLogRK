#include "RemoteLogRK.h"

SYSTEM_THREAD(ENABLED);

retained uint8_t remoteLogBuf[2560];
RemoteLog remoteLog(remoteLogBuf, sizeof(remoteLogBuf));

retained RemoteLogEventRetained remoteLogEventRetained;
RemoteLogEventServer remoteLogEventServer(&remoteLogEventRetained, "debugLog");

SerialLogHandler serialLog;

void setup() {
    waitFor(Serial.isConnected, 10000);

    remoteLog.withServer(&remoteLogEventServer).setup();
}

void loop() {
    remoteLog.loop();

    {
        static unsigned long lastLog = 0;
        static int counter = 0;

        if (millis() - lastLog >= 5000) {
            lastLog = millis();
            Log.info("counter=%d memory=%u", ++counter, System.freeMemory());
        }
    }
}
