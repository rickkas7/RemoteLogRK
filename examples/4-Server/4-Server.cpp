#include "RemoteLogRK.h"

SYSTEM_THREAD(ENABLED);

retained uint8_t remoteLogBuf[2560];
RemoteLog remoteLog(remoteLogBuf, sizeof(remoteLogBuf));

const uint16_t TCP_SERVER_PORT = 5010;
const size_t MAX_CLIENTS = 3;

SerialLogHandler serialLog;

void setup() {
    waitFor(Serial.isConnected, 10000);

    remoteLog.withServer(new RemoteLogTCPServer(TCP_SERVER_PORT, MAX_CLIENTS));

    remoteLog.setup();
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
