#include "RemoteLogRK.h"

RemoteLog *RemoteLog::instance;

static Logger _log("loclog");

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
    servers.push_back(server);
    return *this;
}


void RemoteLog::setup() {
    // Create the mutex, used to protect the retained structure from
    // simultaneous access
    os_mutex_create(&mutex);

    // Call setup operation for registered servers (added using withServer())
    serverOperation(RemoteLogServer::OperationCode::SETUP);

	// Add this handler into the system log manager
	LogManager::instance()->addHandler(this);

    // Add a reset system event handler. This is used so the TCP server can close the connection
    // on system reset, so the TCP connections will close.
    System.on(reset, systemEventHandlerStatic);
}

void RemoteLog::loop() {
    serverOperation(RemoteLogServer::OperationCode::LOOP);
}

void RemoteLog::serverOperation(RemoteLogServer::OperationCode operationCode, void *data) {
    for(auto it = servers.begin(); it != servers.end(); it++) {
        (*it)->operation(operationCode, data);
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
    if (recursionCount == 0) {
        // Be sure to not add any logging commands in this block, as it could lead
        // to infinite recursion.
        WITH_LOCK(*this) {
            getBufData()[getBufHeader()->writeIndex++ % getBufDataLen()] = c;
        } 
    }

    return 1;
}

void RemoteLog::systemEventHandler(system_event_t event, int data, void* moreData) {
    if (event == reset) {
        serverOperation(RemoteLogServer::OperationCode::RESET);
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

void RemoteLogTCPServer::operation(OperationCode operationCode, void *data) {
    if (operationCode == OperationCode::LOOP) {
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
    else if (operationCode == OperationCode::RESET) {
        closeSessions();
    }   
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
            // Incrementing the refCount prevents infinitely recursive logging messages
            // if any code here logs, while reading from the log and writing to the 
            // TCP stream.
            RemoteLog::getInstance()->recursionLock();

            uint8_t *readBuf;
            size_t readBufLen = 512;

            if (RemoteLog::getInstance()->readNoCopy(readIndex, readBuf, readBufLen)) {
                size_t amountWritten = client.write(readBuf, readBufLen, 0);
                if (amountWritten > 0) {
                    readIndex += amountWritten;
                }
            }

            RemoteLog::getInstance()->recursionUnlock();
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

void RemoteLogUDPMulticastServer::operation(OperationCode operationCode, void *data) {
    if (operationCode == OperationCode::LOOP) {
        if (WiFi.ready()) {
            if (!wifiReady) {
                // Wi-Fi is now up, initialize listener
                _log.info("Wi-Fi up");
                udp.begin(0);

                wifiReady = true;
            }

            RemoteLog::getInstance()->recursionLock();
            size_t dataLen = bufLen;
            if (RemoteLog::getInstance()->readLines(readIndex, buf, dataLen)) {
                udp.sendPacket(buf, dataLen, multicastAddr, port);
            }
            RemoteLog::getInstance()->recursionUnlock();
        }
        else {
            if (wifiReady) {
                // Wi-Fi is now down, close existing sessions
                _log.info("Wi-Fi down");

                wifiReady = false;
            }
        }
    }
}

#endif // Wiring_WiFi

RemoteLogEventServer::RemoteLogEventServer(RemoteLogEventRetained *retainedData, const char *eventName) : retainedData(retainedData), eventName(eventName) {

    if (retainedData->magic != RETAINED_MAGIC ||
        retainedData->version != 1 ||
        retainedData->structSize != sizeof(RemoteLogEventRetained)) {
        // Initialize retained data
        retainedData->magic = RETAINED_MAGIC;
        retainedData->version = 1;
        retainedData->structSize = sizeof(RemoteLogEventRetained);
        retainedData->reserved1 = 0;
        retainedData->readIndex = 0;
        retainedData->reserved2 = 0;
    }
}

RemoteLogEventServer::~RemoteLogEventServer() {

}

void RemoteLogEventServer::operation(OperationCode operationCode, void *data) {
    if (operationCode == OperationCode::LOOP) {
        if (stateHandler) {
            stateHandler(*this);
        }
    }
}


void RemoteLogEventServer::stateWaitForMessage() {
    // Also check for connected, so we don't dequeue a message
    // if there is no connection, to minimize the risk that
    // we lose a message since buf is not retained.
    if (!Particle.connected()) {
        return;
    }

    RemoteLog::getInstance()->recursionLock();
    size_t dataLen = sizeof(buf) - 1;
    if (RemoteLog::getInstance()->readLines(retainedData->readIndex, (uint8_t *)buf, dataLen)) {
        // readLines does not create a c-string so do that here
        buf[dataLen] = 0;
        stateHandler = &RemoteLogEventServer::stateTryPublish;
    }
    RemoteLog::getInstance()->recursionUnlock();
}

void RemoteLogEventServer::stateTryPublish() {
    if (!Particle.connected() || millis() - lastPublish < 1010) {
        // Not connected or published too recently
        return;
    }

    lastPublish = millis();
    publishFuture = Particle.publish(eventName, buf, PRIVATE);
    
    stateHandler = &RemoteLogEventServer::stateFutureWait;
}

void RemoteLogEventServer::stateFutureWait() {
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
