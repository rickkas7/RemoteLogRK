#ifndef __REMOTELOGRK_H
#define __REMOTELOGRK_H

// Repository: https://github.com/rickkas7/RemoteLogRK
// License: MIT

#include "Particle.h"

#include <atomic>
#include <vector>

/**
 * @brief The maximum number of servers you can add using withServer
 * 
 * Be careful modifying as ot changes the size of size of RemoteLogBufferHeader.
 */
const size_t REMOTELOG_MAX_SERVERS = 4;

/**
 * @brief Structure typically stored in retained memory
 * 
 * Recommended size 2.5K (2560 bytes). Larger is better. Can be stored in regular
 * RAM if desired. Should not be smaller than 256 bytes. Can't be larger than
 * 65535 bytes as the size is stored in a uint16_t.
 * 
 * The RemoteLogBufHeader structure (not including data) cannot exceed 256 bytes
 * as the headerSize member is a uint8_t. It's currently 32 bytes.
 */
typedef struct { // 32 bytes
    /**
     * @brief magic bytes, RemoteLog::BUF_MAGIC = 0x312ad071
     */
    uint32_t    magic;

    /**
     * @brief version is 1
     */
    uint8_t     version;

    /**
     * @brief sizeof(RemoteLogBufHeader), currently 32. Limited to 255 because of uint8_t
     */
    uint8_t     headerSize;

    /**
     * @brief Total size of buffer including RemoteLogBufHeader and data.
     */
    uint16_t    bufLen;

    /**
     * @brief Index into data buffer to write to next. 
     * 
     * This can be larger than the buffer size as it is always taken modulo buffer size. This happens when the
     * buffer wraps around to the beginning.
     */
    size_t      writeIndex;

    /**
     * @brief Index into data buffer to read from for each server. 
     * 
     * This can be larger than the buffer size as it is always taken modulo buffer size. This happens when the
     * buffer wraps around to the beginning.
     */
    size_t      readIndexes[REMOTELOG_MAX_SERVERS];

    /**
     * @brief Reserved for future use, currently always 0.
     */
    uint32_t    reserved;
    // Data goes here
} RemoteLogBufHeader;

/**
 * @brief Abstract base class for all server types (RemoteLogTCPServer, RemoteLogUDPMulticastServer, etc.)
 */
class RemoteLogServer {
public:
    /**
     * @brief Base class constructor. Doesn't do anything
     */
    RemoteLogServer() {};

    /**
     * @brief This class is not typically deleted, because you can't unregister one
     */
    virtual ~RemoteLogServer() {};

    /**
     * @brief Perform setup operations
     * 
     * Subclasses can override this if they have tasks to perform at setup.
     */
    virtual void setup() {};

    /**
     * @brief Perform loop operations
     * 
     * @param readIndex The read index for this server. Not all servers use this, but for the event 
     * server, for example, it prevents all of the events in the buffer from being resent after
     * a reset since the readIndex parameter is preserved in retained memory.
     * 
     * Subclasses can override this if they have tasks to perform at loop time. This is almost common.
     */
    virtual void loop(size_t &readIndex) {};

    /**
     * @brief Reset is about to occur, do things like disconnect sockets 
     * 
     * The TCP server uses this to disconnect the sockets, otherwise the clients won't know the
     * server went away and will leave the window open, but never receive any more data.
     */
    virtual void reset() {};

protected:
    /**
     * @brief This class is not copyable
     */
    RemoteLogServer(const RemoteLogServer&) = delete;

    /**
     * @brief This class is not copyable
     */
    RemoteLogServer& operator=(const RemoteLogServer&) = delete;

};

/**
 * @brief Remote logging class. Typically created a single global variable.
 * 
 * You can only have one instance of this class per application.
 */
class RemoteLog : public StreamLogHandler, public Print {
public:
    /**
     * @brief Constructor
     * 
     * @param buf Buffer to store log messages. Often uses retained memory, but not required.
     * 
     * @param bufLen Buffer length. Recommended size is 2560 or more. Minimum size is 256 bytes.
     * 
     * @param level Default log level. Default is is LOG_LEVEL_INFO. Using LOG_LEVEL_TRACE may
     * results in an excessive number of log messages.
     * 
     * @param filters Optional log category filters to control the verbosity by category.
     * 
     * There is no default constructor. You must specify the parameters at construction. You
     * typically instantiate this class as a global variable.
     */
    RemoteLog(uint8_t *buf, size_t bufLen, LogLevel level = LOG_LEVEL_INFO, LogCategoryFilters filters = {});

    /**
     * @param Destructor
     * 
     * As this class is normally created as a global variable, it's typically never deleted.
     */
    virtual ~RemoteLog();

    /**
     * @brief Adds a specific server subclass
     * 
     * By default, the RemoteLog doesn't send the data anywhere. You need to associate it
     * with a specific server class such as RemoteLogTCPServer or RemoteLogUDPMulticastServer
     * depending on what you want to do with the log messages. This method registers the
     * server to add.
     * 
     * You can add multiple servers if desired. You must add the servers before calling
     * the setup() method. You cannot remove servers once added. Adding servers after setup
     * is not supported, either.
     * 
     * The maximum number of servers you can add is REMOTELOG_MAX_SERVERS.
     */
    RemoteLog &withServer(RemoteLogServer *server);

    /**
     * @brief You must call setup() from the app setup().
     * 
     * Call withServer() to add all servers before setup()!
     */
    void setup();

    /**
     * @brief You must call loop() from the app loop()
     * 
     * Log messages are queued and processed only during loop() so you should use a buffer
     * big enough to make sure you don't lose messages.
     */
    void loop();

	/**
	 * @brief Virtual override in class Print for the StreamLogHandler to write data to the log
	 */
    virtual size_t write(uint8_t);

    /**
     * @brief Get pointer to the log message buffer as a RemoteLogBufHeader *
     * 
     * This structure is initialized during object construction so it should always be valid.
     */
    RemoteLogBufHeader *getBufHeader() { return (RemoteLogBufHeader *)buf; };

    /**
     * @brief Gets a pointer to the data area of the log message buffer, right after the RemoteLogBufHeader.
     */
    uint8_t *getBufData() { return &buf[sizeof(RemoteLogBufHeader)]; };

    /**
     * @brief Gets the number of bytes of log message data available, taking into account the RemoteLogBufHeader.
     */
    size_t getBufDataLen() const { return bufLen - sizeof(RemoteLogBufHeader); };

    /**
     * @brief Read log messages without copying
     * 
     * @param readIndex Used to keep track of the point you are reading from. Set to 0 on initial
     * call. It will be updated internally, however you must increment it by the amount you
     * have consumed before calling again. See below.
     * 
     * @param readBuf Filled in with a pointer to a uint8_t where the data resides. 
     * 
     * @param readBufLen On input, the maximum number of bytes you want. On output, the
     * number of bytes available.
     * 
     * @return true if there is data available, false if not. If false is returned, readBufLen
     * will also be set to 0.
     * 
     * This method is designed to efficiently handle writing to TCP streams. It may be easier
     * to use readLines() for other applications. The reads will be not be aligned to lines
     * with this function, and individual log messages may be split into two pieces. The idea
     * is that you call readNoCopy() which returns a pointer to the internal buffer in retained
     * memory along with the amount of data you available, limited to the amount you want.
     * Since the buffer is circular, it will also be limited to the point where the buffer
     * wraps around. You then use this data, such as writing to a TCP stream, or a file on a
     * file system. For things that may not consume all of the data, such as TCP with a full
     * buffer and no blocking, you don't have to use all of the data. Increment readIndex by
     * the amount you consumed, up to readBufLen, if you've consumed the entire buffer.
     * 
     * Note: You must lock() and unlock() this object surrounding a call to readNoCopy(). It's
     * not built into this function because you don't want to release it until you've consumed
     * the data you are planning to consume. In the TCP example above, you'd lock surrounding
     * the calls to both readNoCopy() and your TCP write() call.
     */
    bool readNoCopy(size_t &readIndex, uint8_t *&readBuf, size_t &readBufLen);

    /**
     * @brief Copy lines out of the buffer
     * 
     * @param readIndex Used to keep track of the point you are reading from. Set to 0 on initial
     * call. Unlike readNoCopy, readLines does increment readIndex.
     * 
     * @param readBuf Pointer to a buffer to copy data to.
     * 
     * @param readBufLen On input, the maximum number of bytes you want. On output, the
     * number of bytes copied.
     * 
     * @param oneLine If true, only copy one line of data. If false (default), copy as many
     * full lines as will fit.
     * 
     * @return true if there is data available, false if not. If false is returned, readBufLen
     * will also be set to 0.
     * 
     * If readBufLen on input is smaller than an entire line, the line will be returned in 
     * incomplete pieces. 
     * 
     * Do not call lock() and unlock() around this call. It's handled internally.
     */
    bool readLines(size_t &readIndex, uint8_t *readBuf, size_t &readBufLen, bool oneLine = false);


    /**
     * @brief Works like readLines, but only returns a single line and has a filter function
     * 
     * @param readIndex Used to keep track of the point you are reading from. Set to 0 on initial
     * call. Unlike readNoCopy, readLines does increment readIndex.
     * 
     * @param readBuf Pointer to a buffer to copy data to.
     * 
     * @param readBufLen On input, the maximum number of bytes you want. On output, the
     * number of bytes copied.
     * 
     * @param allowPartialLine If true, then a partial line that is longer than readBufLen is 
     * returned. If false, false is returned.
     * 
     * @param filter Function to filter input
     * 
     * @return true if there is data available, false if not. If false is returned, readBufLen
     * will also be set to 0.
     * 
     * The filter function has this prototype and can be a lambda:
     * 
     * bool myFilter(uint8_t *readBuf, size_t &readBufLen)
     * 
     * - readBuf is the beginning of the line, and will be equal to readBuf passed into readLineFilter()
     * - readBufLen is the length of the line. The line will not contain a trailing null!
     * 
     * If the filter function returns true, the buffer will be returned to the caller of readLineFilter().
     * Your filter function can modify readBuf and readBufLen in place if desired.
     * 
     * If the filter function returns false, then the caller of readLineFilter() will get a false response
     * as well. 
     */
    bool readLineFilter(size_t &readIndex, uint8_t *readBuf, size_t &readBufLen, bool allowPartialLine, std::function<bool(uint8_t *readBuf, size_t &readBufLen)> filter);


    /**
     * @brief Mutex lock, used to safely access the buffer
     * 
     * Since the buffer is written to by the write() method which may be called from another
     * thread, a mutex is needed to safely access it from loop() as well.
     * 
     * Be sure to balance every call to lock() with an unlock(). Avoid locking for extended
     * periods of time as this will block logging from other threads, which may cause other
     * threads to block.
     */
    void lock() { os_mutex_lock(mutex); };

    /**
     * @brief Mutex lock, used to safely access the buffer
     * 
     * The trylock() method returns true if the mutex was locked (and must be unlocked later)
     */
    bool trylock() { return os_mutex_trylock(mutex)==0; };

    /**
     * @brief Mutex unlock, used release the mutex obtained by lock() or trylock()
     */
    void unlock() { os_mutex_unlock(mutex); };

    /**
     * @brief System event handler callback
     * 
     * The RemoteLog registers for reset events and passes them to servers. This is used
     * by the RemoteLogTCPServer to stop the connection before reset. Otherwise, the 
     * client may not realize that the server has gone away.
     */
    void systemEventHandler(system_event_t event, int data, void* moreData);

    /**
     * @brief System event handler callback (static)
     * 
     * Finds the object instance using getInstance() as it's a singleton.
     */
    static void systemEventHandlerStatic(system_event_t event, int data, void* moreData);

    /**
     * @brief Get the singleton instance of this class
     * 
     * This object is normally instantiated as a single instance global variable. The
     * constructor saves the object pointer so it can be retrieved using getInstance().
     * This necessarily means you can only have one instance of this class, but it
     * doesn't make sense to have more than one RemoteLog. You can have multiple servers.
     */
    static RemoteLog *getInstance() { return instance; };

    /**
     * @brief Magic bytes stored in the retained memory structure
     */
    static const uint32_t BUF_MAGIC = 0x312ad071;

protected:
    /**
     * @brief This class is not copyable
     */
    RemoteLog(const RemoteLog&) = delete;

    /**
     * @brief This class is not copyable
     */
    RemoteLog& operator=(const RemoteLog&) = delete;

    /**
     * @brief Pointer to the buffer
     * 
     * Begins with a RemoteLogBufHeader and has a circular buffer of data after it
     */
    uint8_t *buf;

    /**
     * @brief Size of the buffer, including both the RemoteLogBufHeader and the data
     */
    size_t bufLen;

    /**
     * @brief Server object instances, added using withServer()
     */
    RemoteLogServer *servers[REMOTELOG_MAX_SERVERS];

    /**
     * @brief Number of servers assigned
     * 
     * 0 = no servers, 1 = index 0 is filled in, ...
     * 
     * Will be at most REMOTELOG_MAX_SERVERS.
     */
    size_t numServers = 0;

    /**
     * @brief Mutex for preventing simultaneous access to the buffer from multiple threads
     */
    os_mutex_t mutex = 0;

    /**
     * @brief Singleton instance of this class
     */
    static RemoteLog *instance;
};

#if Wiring_WiFi

class RemoteLogTCPServer; // Forward declaration

/**
 * @brief Session object - one of these for each client connected to the RemoteLogTCPServer
 * 
 * Lifetime is managed by RemoteLogTCPServer, you never allocate or free these objects
 */
class RemoteLogTCPSession {
protected:
    /**
     * @brief State information for a single TCP session
     * 
     * @param server The server this session came from
     * 
     * @param client The TCPClient that the session was accepted on. 
     */
    RemoteLogTCPSession(RemoteLogTCPServer *server, TCPClient client);

    /**
     * @brief Destructor - this object is deleted when the session is closed
     */
    virtual ~RemoteLogTCPSession();

    /**
     * @brief Handle sending data on this session
     */
    void loop();

    /**
     * @brief Returns true if the TCP connection is closed but this object has not been deleted yet
     */
    bool isDone();


protected:
    /**
     * @brief The RemoteLogTCPServer this session is associated with. Set in the constructor.
     */
    RemoteLogTCPServer *server;

    /**
     * @brief The TCPClient the session is using to communicate over. Set in the constructor.
     */
    TCPClient client;  

    /**
     * @brief A numeric id used to identify this session. Used for logging.
     */
    int id = 0; 

    /**
     * @brief System.millis() (64-bit) time value when data was last received on this session
     */
    uint64_t recvTime = 0;

    /**
     * @brief Next location in the log buffer to read from
     */
    size_t readIndex = 0;

    /**
     * @brief The last id value used. Set so the first session is 1, and increases for each new session
     */
    static int lastId;

    friend class RemoteLogTCPServer;
};

/**
 * @brief Server class for sending out log messages from a TCP server (Wi-Fi only)
 * 
 * This is typically uses so computers on the local LAN can connect to the TCP server
 * using nc (netcat) and see the recent logs, along with live tail.
 * 
 * There is no authentication! Any user on the network can connect to the device.
 */
class RemoteLogTCPServer : public RemoteLogServer {
public:
    /**
     * @brief Constructor
     * 
     * @param port The TCP port to listen on
     * 
     * @param maxConn The maximum number of connections that can be listed on. Device OS
     * also sets a limit on the number of sockets available.
     */
    RemoteLogTCPServer(uint16_t port, size_t maxConn);

     /**
     * @brief Destructor. 
     * 
     * This object is not typically deleted as you can't unregister a server, so deleting
     * it would cause a dangling pointer.
     */   
    virtual ~RemoteLogTCPServer();

    /**
     * @brief Close all sessions. Frees sockets and memory.
     * 
     * This calls client.stop() on the client objects. This is done when Wi-Fi goes down, and also right before
     * System.reset() resets. The reason is that if the device resets without closing the TCP connection, clients
     * will think they are still connected to the server, but won't receive any new messages. By doing a close
     * before reset, it assumes that TCP sessions will close gracefully.
     */
    void closeSessions();

    /**
     * @brief Sets a session timeout
     * 
     * @param milliseconds Timeout in milliseconds (default is 0, no timeout)
     * 
     * If enabled, if the other side does not send data for this long, the session will be closed. Normally, there is no 
     * need for the other side to send data, but using a session timeout can be used as a keep-alive to make sure the 
     * connection is really up.
     */
    RemoteLogTCPServer &withSessionTimeout(unsigned long milliseconds) { sessionTimeout = milliseconds; return *this; };

    /**
     * @brief Sets a session timeout
     * 
     * @param timeout Timeout as a chrono literal. You can pass in values like 30s (30 seconds) or 5min (5 minutes).
     * 
     * If enabled, if the other side does not send data for this long, the session will be closed. Normally, there is no 
     * need for the other side to send data, but using a session timeout can be used as a keep-alive to make sure the 
     * connection is really up.
     */
    RemoteLogTCPServer &withSessionTimeout(std::chrono::milliseconds timeout) { sessionTimeout = timeout.count(); return *this; };

    /**
     * @brief Gets the session timeout value in milliseconds
     */
    unsigned long getSessionTimeout() const { return sessionTimeout; };

    /**
     * @brief Perform loop time operations such as reading and writing sockets
     */
    void loop(size_t &readIndex);

    /**
     * @brief Reset is about to occur, disconnect sockets 
     */
    virtual void reset();

protected:
    /**
     * @brief The TCP port to listen on
     */
    uint16_t port;

    /**
     * @brief The maximum number of simultaneous connections
     */
    size_t maxConn;

    /**
     * @brief The Wiring TCP server (listener) object
     */
    TCPServer server;

    /**
     * @brief Vector of sessions, one per active connection
     */
    std::vector<RemoteLogTCPSession *>sessions;

    /**
     * @brief How long to wait before timing out sessions (0 = never). See withSessionTimeout().
     */
    unsigned long sessionTimeout = 0;

    /**
     * @brief Set to true when WiFi.ready() is true
     * 
     * The TCP server is bound only after WiFi.ready() is true, and is
     * reinitialized after WiFi goes down.
     */
    bool wifiReady = false;
};


/**
 * @brief Server class for sending out log messages by UDP multicast (Wi-Fi only)
 */
class RemoteLogUDPMulticastServer : public RemoteLogServer {
public:
    /**
     * @brief Constructor
     * 
     * @param multicastAddr The UDP multicast address, for example 239.1.1.235. This is not
     * a normal public or private IP address!
     * 
     * @param port The UDP port to use. Both the multicast address and port must match what
     * the side receiving the multicast is expecting!
     * 
     * @param bufLen The maximum UDP packet size. Default is 512.
     */
    RemoteLogUDPMulticastServer(IPAddress multicastAddr, uint16_t port, size_t bufLen = 512);

    /**
     * @brief Destructor. 
     * 
     * This object is not typically deleted as you can't unregister a server, so deleting
     * it would cause a dangling pointer.
     */
    virtual ~RemoteLogUDPMulticastServer();

    /**
     * @brief Perform loop time operations such as reading and writing sockets
     */
    void loop(size_t &readIndex);


protected:
    /**
     * @brief The multicast address to send to
     * 
     * Note that this must be a specific multicast IP address 224.0.0.0 to 239.255.255.255, not
     * a regular local IP address.
     */
    IPAddress multicastAddr;

    /**
     * @brief The UDP port to send to.
     */
    uint16_t port;

    /**
     * @brief The Wiring UDP object used to send UDP data
     */
    UDP udp;

    /**
     * @brief Set to true when WiFi.ready() is true
     * 
     * The UDP object is bound using UDP.begin() only after WiFi.ready() is true, and is
     * reinitialized after WiFi goes down.
     */
    bool wifiReady = false;

    /**
     * @brief The packet buffer. Allocated in the constructor.
     */
    uint8_t *buf;

    /**
     * @brief The size of the packet buffer. Set in the constructor.
     */
    size_t bufLen;
};

#endif // Wiring_WiFi

/**
 * @brief Log server module for writing to a syslog server over UDP
 * 
 * The syslog server can be on your local network, or cloud-hosted, like Solarwinds Papertrail.
 */
class RemoteLogSyslogUDP : public RemoteLogServer {
public:
    /**
     * @brief Constructor
     * 
     * @param bufLen The maximum UDP packet size. Default is 256. It should not be smaller
     * than this, because the first 128 bytes of is used as temporary storage for formatting
     * the syslog data before the actual event date is shifted in the buffer.
     * 
     * If you use this constructor you must set the hostname and port using 
     */
    RemoteLogSyslogUDP(size_t bufLen = 256);

    /**
     * @brief Constructor
     * 
     * @param hostname The UDP hostname to send to.
     * 
     * @param port The UDP port to send to.
     * 
     * @param bufLen The maximum UDP packet size. Default is 256. It should not be smaller
     * than this, because the first 128 bytes of is used as temporary storage for formatting
     * the syslog data before the actual event date is shifted in the buffer.
     */
    RemoteLogSyslogUDP(const char *hostname, uint16_t port, size_t bufLen = 256);

    /**
     * @brief Destructor. 
     * 
     * This object is not typically deleted as you can't unregister a server, so deleting
     * it would cause a dangling pointer.
     */
    virtual ~RemoteLogSyslogUDP();

    /**
     * @brief Sets the hostname and port of the UDP syslog server
     * 
     * @param hostname The UDP hostname to send to.
     * 
     * @param port The UDP port to send to.
     */
    RemoteLogSyslogUDP &withHostnameAndPort(const char *hostname, uint16_t port);

    /**
     * @brief Sets the callback to get the device name, used in the syslog packet
     * 
     * @param deviceNameCallback The callback function or C++11 lambda.
     * 
     * The callback function or C++ lambda should have the prototype:
     * 
     * bool callback(String &deviceName);
     * 
     * The deviceName should be filled in, if known, and return true. If the device name is not yet known,
     * then return false. This will prevent syslog messages from going out, however, so you may want to
     * return some default value and return true instead.
     */
    RemoteLogSyslogUDP &withDeviceNameCallback(std::function<bool(String&)> deviceNameCallback) { this->deviceNameCallback = deviceNameCallback; return *this; };

    /**
     * @brief Sets the minimum period between UDP sends (default: 100 milliseconds)
     * 
     * It's possible to overload the UDP stack causing packets to be dropped. Checking the return value from
     * UDP.sendPacket would help, however there is no good way to put the data back into the buffer after
     * removing it. 
     * 
     * Adding rate limiting can also help slow down transmission if runaway recursion occurs. The value could
     * be make even higher (1000 ms) to help protect against this on cellular devices in particular.
     * 
     * @param valueMs the value in milliseconds to set the sendPeriodMs to.
     */
    RemoteLogSyslogUDP &withMinSendPeriodMs(unsigned long valueMs) { minSendPeriodMs = valueMs; return *this; };

    /**
     * @brief Perform loop time operations such as reading and writing sockets
     */
    void loop(size_t &readIndex);

protected:
    /**
     * @brief UDP send rate limit (milliseconds)
     * 
     * This is a safety net to avoid sending data too quickly. Since UDP data is not acknowledged, there's no
     * way to know if it's been discarded. This can be set to 0 for no rate limiting.
     */
    unsigned long minSendPeriodMs = 100;

    /**
     * @brief The millis value of the last send used for rate limiting
     */
    unsigned long lastSendMs = 0;

    /**
     * @brief The hostname to send to 
     * 
     * This is resolved by DNS once at startup, then the cached address is used. If you specify
     * a dotted decimal IPv4 address in hostname (10.1.2.3, for example), it's parsed directly
     * without network access.
     */
    String hostname;

    /**
     * @brief The IPv4 address of the remote host
     */
    IPAddress remoteAddr;

    /**
     * @brief The UDP port to send to
     */
    uint16_t port = 0;

    /**
     * @brief The function to call to find the name of this device set using withDeviceNameCallback
     */
    std::function<bool(String&)> deviceNameCallback = 0;

    /**
     * @brief The Wiring UDP object used to send UDP data
     */
    UDP udp;

    /**
     * @brief true if Celluar.ready() or WiFi.ready() is true
     */
    bool networkReady = false;

    /**
     * @brief The packet buffer. Allocated in the constructor.
     */
    uint8_t *buf;

    /**
     * @brief The size of the packet buffer. Set in the constructor.
     */
    size_t bufLen;
};



/**
 * @brief Sends out debug logs as Particle events
 * 
 * Works on both cellular and Wi-Fi. Automatically meters out data at 1 second
 * intervals to avoid event throttling. Sends multiple messages in a single
 * publish up to the maximum publish size of 622 bytes if there are multiple
 * messages queued. However, if there is only one available, that one will be
 * sent immediately to keep the events in approximately real-time.
 */
class RemoteLogEventServer : public RemoteLogServer {
public:
    /**
     * @brief Constructor
     * 
     * @param eventName The event name to use for the publish
     */
    RemoteLogEventServer(const char *eventName);

    /**
     * @brief Destructor. 
     * 
     * This object is not typically deleted as you can't unregister a server, so deleting
     * it would cause a dangling pointer.
     */
    virtual ~RemoteLogEventServer();

    /**
     * @brief Perform loop time operations such as reading and writing sockets
     * 
     * Calls the state handler in stateHandler.
     */
    void loop(size_t &readIndex);

protected:
    /**
     * @brief State handler to wait for cloud connected and messages to send. Called from loop().
     * 
     * If cloud connected and there is a message, the message is consumed and copied into buf.
     * 
     * Next state: stateTryPublish
     */
    void stateWaitForMessage(size_t &readIndex);

    /** 
     * @brief If still connected to the cloud and it has been at least 1010 milliseconds, publish
     * 
     * The publish is non-blocking using a future.
     * 
     * Previous state: stateWaitForMessage
     * Next state: stateFutureWait
     */
    void stateTryPublish(size_t &readIndex);

    /**
     * @brief Wait for the publish to succeed or fail
     * 
     * Next state: stateWaitForMessage (success)
     *   or stateTryPublish (failure)
     */
    void stateFutureWait(size_t &readIndex);

    /**
     * @brief The event name to publish, passed to the constructor
     */
    String eventName;

    /**
     * @brief Buffer to hold the event to be published
     * 
     * It's copied into a buffer because otherwise it would need to lock the main circular buffer
     * during the entire publish operation, which would take way too long.
     */
    char buf[particle::protocol::MAX_EVENT_DATA_LENGTH + 1]; // 622 bytes + null terminator

    /**
     * @brief The millis() counter at the time of the last publish
     * 
     * This is used to prevent publishing more than once every 1010 milliseconds.
     */
    unsigned long lastPublish = 0;

    /**
     * @brief The Future used to determine if the publish has completed yet
     */
    particle::Future<bool> publishFuture;

    /**
     * @brief State handler, called from loop
     */
    std::function<void(RemoteLogEventServer&, size_t &readIndex)> stateHandler = &RemoteLogEventServer::stateWaitForMessage;
};


#endif /* __REMOTELOGRK_H */
