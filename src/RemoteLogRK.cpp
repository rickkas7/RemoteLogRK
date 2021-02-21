#include "RemoteLogRK.h"

RemoteLog *RemoteLog::instance;

static Logger _log("remlog");

RemoteLog::RemoteLog(uint8_t *buf, size_t bufLen, LogLevel level, LogCategoryFilters filters) : StreamLogHandler(*this, level, filters), buf(buf), bufLen(bufLen) {
    // Initialize retained memory structure
    RemoteLogBufHeader *hdr = getBufHeader();
    if (hdr->magic != BUF_MAGIC || 
        hdr->version != 1 || 
        hdr->headerSize != sizeof(RemoteLogBufHeader) ||
        hdr->bufLen != bufLen) {
        // Initialize structure
        hdr->magic = BUF_MAGIC;
        hdr->version = 1;
        hdr->headerSize = sizeof(RemoteLogBufHeader);
        hdr->bufLen = bufLen;
        hdr->writeIndex = 0;
        hdr->reserved = 0;
    }

    instance = this;
}

RemoteLog::~RemoteLog() {

}

RemoteLog &RemoteLog::withServer(RemoteLogServer *server) {
    if ((numServers + 1) < REMOTELOG_MAX_SERVERS) {
        servers[numServers++] = server;
    }
    return *this;
}


void RemoteLog::setup() {
    // Create the mutex, used to protect the retained structure from
    // simultaneous access
    os_mutex_create(&mutex);

    // Call setup operation for registered servers (added using withServer())
    for(size_t ii = 0; ii < numServers; ii++) {
        servers[ii]->setup();
    }

	// Add this handler into the system log manager
	LogManager::instance()->addHandler(this);

    // Add a reset system event handler. This is used so the TCP server can close the connection
    // on system reset, so the TCP connections will close.
    System.on(reset, systemEventHandlerStatic);
}

void RemoteLog::loop() {
    for(size_t ii = 0; ii < numServers; ii++) {
        RemoteLogBufHeader *hdr = getBufHeader();
        servers[ii]->loop(hdr->readIndexes[ii]);
    }
}


bool RemoteLog::readNoCopy(size_t &readIndex, uint8_t *&readBuf, size_t &readBufLen) {
    size_t bufDataLen = getBufDataLen();
    RemoteLogBufHeader *hdr = getBufHeader();
    
    // Log.info("readNoCopy readIndex=%u writeIndex=%u", readIndex, hdr->writeIndex);

    if ((hdr->writeIndex - readIndex) > bufDataLen) {
        // readOffset is too far back, limit to available data
        readIndex = hdr->writeIndex - bufDataLen;
    }
    if (readIndex >= hdr->writeIndex) {
        // Nothing to read
        readBufLen = 0;
        return false;
    }


    if ((hdr->writeIndex - readIndex) < readBufLen) {
        // Less data available than the buffer size
        readBufLen = hdr->writeIndex - readIndex;
    }

    if ((readIndex % bufDataLen + readBufLen) > bufDataLen) {
        // Data would wrap around to beginning of buffer, limit to the contiguous block
        readBufLen = bufDataLen - readIndex % bufDataLen;
    }

    readBuf = &getBufData()[readIndex % bufDataLen];

    return true;
}

bool RemoteLog::readLines(size_t &readIndex, uint8_t *readBuf, size_t &readBufLen, bool oneLine) {
    WITH_LOCK(*this) {
        size_t bufDataLen = getBufDataLen();
        RemoteLogBufHeader *hdr = getBufHeader();
        
        // Log.info("readNoCopy readIndex=%u writeIndex=%u", readIndex, hdr->writeIndex);

        if ((hdr->writeIndex - readIndex) > bufDataLen) {
            // readOffset is too far back, limit to available data
            readIndex = hdr->writeIndex - bufDataLen;
        }
        if (readIndex >= hdr->writeIndex) {
            // Nothing to read
            readBufLen = 0;
            return false;
        }

        // Copy data
        size_t index;
        size_t startOfLine = 0;
        for(index = 0; (readIndex + index) < hdr->writeIndex && index < readBufLen; index++) {
            readBuf[index] = getBufData()[(readIndex + index) % bufDataLen];
            if (readBuf[index] == '\n') {
                startOfLine = index + 1;
                if (oneLine) {
                    // Caller only wants one line, and we have one line now
                    index++;
                    break;
                }
            }
        }
        if (startOfLine < index) {
            // We have a partial line
            if (startOfLine == 0) {
                // The line has not been fully written to the buffer yet
                if (index < readBufLen) {
                    // Wait for it to be fully written. The index check makes
                    // sure a line longer than readBufLen will be split instead
                    // of getting stuck here forever.
                    readBufLen = 0;
                    return false;
                }
                // Line is longer than readBufLen so include the whole thing
                // instead of splitting at LF
                startOfLine = index;
            }
            readBufLen = startOfLine;
            readIndex += startOfLine;
        }
        else {
            readBufLen = index;
            readIndex += index;
        }
    }
    return true;
}


size_t RemoteLog::write(uint8_t c) {
    WITH_LOCK(*this) {
        getBufData()[getBufHeader()->writeIndex++ % getBufDataLen()] = c;
    } 

    return 1;
}

void RemoteLog::systemEventHandler(system_event_t event, int data, void* moreData) {
    if (event == reset) {
        for(size_t ii = 0; ii < numServers; ii++) {
            servers[ii]->reset();
        }
    }
}

// [static]
void RemoteLog::systemEventHandlerStatic(system_event_t event, int data, void* moreData) {
    getInstance()->systemEventHandler(event, data, moreData);
}

#if Wiring_WiFi

RemoteLogTCPServer::RemoteLogTCPServer(uint16_t port, size_t maxConn) : port(port), maxConn(maxConn), server(port) {

}

RemoteLogTCPServer::~RemoteLogTCPServer() {
}

void RemoteLogTCPServer::loop(size_t &readIndex) {
    if (WiFi.ready()) {
        if (!wifiReady) {
            // Wi-Fi is now up, initialize listener
            // _log.info("Wi-Fi up");
            server.begin();

            wifiReady = true;
        }

        // Handle new sessions
        TCPClient newClient = server.available();
        if (newClient.connected()) {
            // Connection established
            if (sessions.size() < maxConn) {
                RemoteLogTCPSession *sess = new RemoteLogTCPSession(this, newClient);
                if (sess) {
                    sessions.push_back(sess);
                }
                else {
                    // Out of memory, drop connection
                    newClient.stop();    
                }
            }
            else {
                // Server busy, drop this connection
                newClient.stop();
            }
        }

        // Clean up sessions
        for(auto it = sessions.begin(); it != sessions.end(); ) {
            RemoteLogTCPSession *sess = *it;
            if (sess->isDone()) {
                it = sessions.erase(it);
                delete sess;
            }
            else {
                it++;
            }
        }

        // Handle existing sessions
        for(auto it = sessions.begin(); it != sessions.end(); it++) {
            (*it)->loop();
        }
    }
    else {
        if (wifiReady) {
            // Wi-Fi is now down, close existing sessions
            // _log.info("Wi-Fi down");
            closeSessions();

            wifiReady = false;
        }
    }
}

void RemoteLogTCPServer::reset() {
    closeSessions();
}

void RemoteLogTCPServer::closeSessions() {
    while(!sessions.empty()) {
        RemoteLogTCPSession *sess = sessions.back();
        sessions.pop_back();

        delete sess;
    }
}

int RemoteLogTCPSession::lastId = 0;

RemoteLogTCPSession::RemoteLogTCPSession(RemoteLogTCPServer *server, TCPClient client) : server(server), client(client) {
    id = ++lastId;
    recvTime = System.millis();
    _log.info("TCP session %d started %s", id, client.remoteIP().toString().c_str());
}

RemoteLogTCPSession::~RemoteLogTCPSession() {
    client.stop();
    _log.info("TCP session %d ended", id);
}

void RemoteLogTCPSession::loop() {
    if (client.connected()) {
        // Send any new data to the client
        WITH_LOCK(*RemoteLog::getInstance()) {
            uint8_t *readBuf;
            size_t readBufLen = 512;

            if (RemoteLog::getInstance()->readNoCopy(readIndex, readBuf, readBufLen)) {
                size_t amountWritten = client.write(readBuf, readBufLen, 0);
                if (amountWritten > 0) {
                    readIndex += amountWritten;
                }
            }
        }

        // Read and discard any received data
        while(client.read() != -1) {
            recvTime = System.millis();
        }

        // Handle timeouts
        if (server->getSessionTimeout()) {
            if (System.millis() - recvTime > server->getSessionTimeout()) {
                _log.info("TCP session %d timeout", id);
                recvTime = System.millis();
                client.stop();
            }
        }
    }
}

bool RemoteLogTCPSession::isDone() {
    return !client.connected();
}


RemoteLogUDPMulticastServer::RemoteLogUDPMulticastServer(IPAddress multicastAddr, uint16_t port, size_t bufLen) : multicastAddr(multicastAddr), port(port), bufLen(bufLen) {
    buf = new uint8_t[bufLen];
}

RemoteLogUDPMulticastServer::~RemoteLogUDPMulticastServer() {
    delete[] buf;
}

void RemoteLogUDPMulticastServer::loop(size_t &readIndex) {
    if (WiFi.ready()) {
        if (!wifiReady) {
            // Wi-Fi is now up, initialize listener
            _log.info("Wi-Fi up");
            udp.begin(0);

            wifiReady = true;
        }

        size_t dataLen = bufLen;
        if (RemoteLog::getInstance()->readLines(readIndex, buf, dataLen)) {
            udp.sendPacket(buf, dataLen, multicastAddr, port);
        }
    }
    else {
        if (wifiReady) {
            // Wi-Fi is now down, close existing sessions
            _log.info("Wi-Fi down");

            wifiReady = false;
        }
    }

}

#endif // Wiring_WiFi

RemoteLogSyslogUDP::RemoteLogSyslogUDP(const char *hostname, uint16_t port, size_t bufLen) : hostname(hostname), port(port), bufLen(bufLen) {
    buf = new uint8_t[bufLen];
    if (buf) {
        buf[0] = 0;
    }
}

RemoteLogSyslogUDP::~RemoteLogSyslogUDP() {
    delete[] buf;
}

RemoteLogSyslogUDP &RemoteLogSyslogUDP::withHostnameAndPort(const char *hostname, uint16_t port) {
    this->hostname = hostname;
    this->port = port;
    this->remoteAddr = IPAddress();
    return *this;
}

void RemoteLogSyslogUDP::loop(size_t &readIndex) {
    if (Network.ready()) {
        if (!networkReady) {
            // Network is now up, initialize listener
            _log.info("Network up");
            udp.begin(0);

            networkReady = true;
        }

        if (!Time.isValid()) {
            // No timestamp yet
            return;
        }

        if (hostname.length() == 0 || port == 0) {
            // Not configured yet
            return;
        }

        if (millis() - lastSendMs < minSendPeriodMs) {
            // Rate limiting for UDP sends
            return;
        }
        lastSendMs = millis();
        
        if (!remoteAddr) {
            int a[4];
            if (sscanf(hostname.c_str(), "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) == 4) {
                // The hostname is a string IP address
                remoteAddr = IPAddress(a[0], a[1], a[2], a[3]);
            }
            else {
#if Wiring_WiFi
                remoteAddr = WiFi.resolve(hostname);
#endif
#if Wiring_Cellular
                remoteAddr = Cellular.resolve(hostname);
#endif
                // TODO: Add support for Ethernet here
            }
            // _log.info("sending to %s (%s) port %d", remoteAddr.toString().c_str(), hostname.c_str(), port);
        }
        if (!remoteAddr) {
            // On failure to get IP address, wait 10 seconds before trying again
            lastSendMs = millis() + 10000;
            return;
        }
        
        String deviceName = "particle";
        if (deviceNameCallback) {
            if (!deviceNameCallback(deviceName)) {
                // We don't have the device name yet, so hold off on posting log messages until we do
                return;
            }
        }
        
        if (remoteAddr && buf[0]) {
            // There was a saved buffer that was not previously sent
            int count = udp.sendPacket(buf, strlen((const char *)buf), remoteAddr, port);
            if (count > 0) {
                // We successfully sent the buffer, get a new line next time
                buf[0] = 0;
            }
            return;
        }

        if (remoteAddr) {
            const size_t PREFIX_SIZE = 128;

            char *logMsg = (char *) &buf[PREFIX_SIZE];
            size_t logMsgLen = bufLen - PREFIX_SIZE - 1;

            if (RemoteLog::getInstance()->readLines(readIndex, (unsigned char *) logMsg, logMsgLen, true)) {
                // Create a null terminated string
                logMsg[logMsgLen] = 0;

                char *cur = logMsg;
                char *parts[4] = {0};
                for(size_t ii = 0; ii < 3; ii++) {
                    parts[ii] = strtok_r(cur, " ", &cur);
                }
                if (parts[2]) {
                    parts[3] = cur;
                }

                int severity;
                const char *category;
                char *preMsg;
                char *msg;
                long timeVal = Time.now();

                if (parts[3]) {
                    // 10050398 app INFO pressure=56
                    // parts[0] = millis timestamp
                    // parts[1] = category
                    // parts[2] = level
                    // parts[3] = message

                    if (strcmp(parts[2], "TRACE") == 0) {
                        severity = 7; // Debug
                    }
                    else if (strcmp(parts[2], "WARN") == 0) {
                        severity = 4; // Warning
                    }
                    else if (strcmp(parts[2], "ERROR") == 0) {
                        severity = 3; // Error
                    }
                    else {
                        severity = 6; // Info
                    }

                    preMsg = parts[0];
                    category = parts[1];
                    if (category[0] == '[') {
                        category++;
                        char *cp = strchr(category, ']');
                        if (cp) {
                            *cp = 0;
                        }
                    }
                    msg = parts[3];

                    unsigned long ms = strtoul(parts[0], NULL, 10);

                    if (ms < millis()) {
                        // Try to use the millis counter to make the timestamp more accurate
                        long deltaSec = (long) (millis() - ms) / 1000; 
                        timeVal -= deltaSec;
                    }
                }
                else {
                    // Just treat the whole thing as the message
                    severity = 6; // info
                    category = "app";
                    preMsg = 0;
                    msg = logMsg;                       
                }

                // This is mostly TIME_FORMAT_ISO8601_FULL, but replaces %z with literal Z
                // as Papertrail doesn't like to have a timezone. Assumption is that you
                // have not called Time.zone() so this will be UTC.
                String timeString = Time.format(timeVal, "%Y-%m-%dT%H:%M:%S");

                char *cp = (char *)buf;
                cp += snprintf(cp, PREFIX_SIZE, "<22>%d %s%s %s %s - - - ",
                    severity, timeString.c_str(), "Z", deviceName.c_str(), category);

                if (preMsg) {
                    cp += sprintf(cp, "%s ", preMsg);
                }

                size_t msgLen = strlen(msg);
                memmove(cp, msg, msgLen);
                cp += msgLen;
                *cp = 0;

                int count = udp.sendPacket(buf, strlen((const char *)buf), remoteAddr, port);
                if (count > 0) {
                    // We successfully sent the buffer, get a new line next time
                    buf[0] = 0;
                }
            }
        }
    }
    else {
        if (networkReady) {
            // Network is now down, close existing sessions
            _log.info("Network down");

            networkReady = false;
        }
    }

}

//
// RemoteLogEventServer
//

RemoteLogEventServer::RemoteLogEventServer(const char *eventName) : eventName(eventName) {

}

RemoteLogEventServer::~RemoteLogEventServer() {

}

void RemoteLogEventServer::loop(size_t &readIndex) {
    if (stateHandler) {
        stateHandler(*this, readIndex);
    }
}


void RemoteLogEventServer::stateWaitForMessage(size_t &readIndex) {
    // Also check for connected, so we don't dequeue a message
    // if there is no connection, to minimize the risk that
    // we lose a message since buf is not retained.
    if (!Particle.connected()) {
        return;
    }

    size_t dataLen = sizeof(buf) - 1;
    if (RemoteLog::getInstance()->readLines(readIndex, (uint8_t *)buf, dataLen)) {
        // readLines does not create a c-string so do that here
        buf[dataLen] = 0;
        stateHandler = &RemoteLogEventServer::stateTryPublish;
    }
}

void RemoteLogEventServer::stateTryPublish(size_t &readIndex) {
    if (!Particle.connected() || millis() - lastPublish < 1010) {
        // Not connected or published too recently
        return;
    }

    lastPublish = millis();
    publishFuture = Particle.publish(eventName, buf, PRIVATE);
    
    stateHandler = &RemoteLogEventServer::stateFutureWait;
}

void RemoteLogEventServer::stateFutureWait(size_t &readIndex) {
    if (!publishFuture.isDone()) {
        // Publish still being attempted
        return;
    }
    
    if (publishFuture.isSucceeded()) {
        // Publish succeeded!
        stateHandler = &RemoteLogEventServer::stateWaitForMessage;
    }
    else {
        // Publish failed. Try again.
        stateHandler = &RemoteLogEventServer::stateTryPublish;
    }
}
