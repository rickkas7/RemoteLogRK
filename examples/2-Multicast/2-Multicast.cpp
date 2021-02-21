#include "RemoteLogRK.h"

SYSTEM_THREAD(ENABLED);

retained uint8_t remoteLogBuf[2560];
RemoteLog remoteLog(remoteLogBuf, sizeof(remoteLogBuf));

// Note: multicastAddr is a multicast address, it should begin with 239.x.x.x or
// some other appropriate multicast address.
// It's not the same as your local IP address (192.168.x.x, for example)!
const IPAddress MULTICAST_ADDR(239,1,1,235);

const uint16_t UDP_PORT = 5010;

SerialLogHandler serialLog;

void setup() {
    waitFor(Serial.isConnected, 10000);

    remoteLog.withServer(new RemoteLogUDPMulticastServer(MULTICAST_ADDR, UDP_PORT, 512));

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
