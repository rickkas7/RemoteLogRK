#ifndef __REMOTELOGRK_H
#define __REMOTELOGRK_H

// Repository: 

#include "Particle.h"

#include <atomic>
#include <vector>

/**
 * @brief Structure typically stored in retained memory
 * 
 * Recommended size 2.5K (2560 bytes). Larger is better. Can be stored in regular
 * RAM if desired. Should not be smaller than 256 bytes. Can't be larger than
 * 65535 bytes as the size is stored in a uint16_t.
 * 
 * The RemoteLogBufHeader structure (not including data) cannot exceed 256 bytes
 * as the headerSize member is a uint8_t. It's currently 16 bytes.
 */
typedef struct { // 16 bytes
    uint32_t    magic;
    uint8_t     version;
    uint8_t     headerSize;
    uint16_t    bufLen;
    size_t      writeIndex;
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
     * @brief Base class destructor. Doesn't do anything.
     */
    virtual ~RemoteLogServer() {};

    /**
     * @brief Operations a server may perform
     * 
     * The operations are differentiated by code because it makes it easy to iterate all servers
     * and pass the appropriate OperationCode. Using a separate method ends up requiring multiple
     * iterators, funky code, or templates.
     */
    enum class OperationCode {
        SETUP,
        LOOP,
        RESET
    };

    /**
     * @brief Subclassed to handle server operations
     */
    virtual void operation(OperationCode code, void *data) = 0;
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
     * @brief Have all servers perform an operation
     * 
     * Iterates the server list and sends operationCode (for example: SETUP, LOOP, or RESET) to each server.
     */
    void serverOperation(RemoteLogServer::OperationCode operationCode, void *data = NULL);

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
     * call. It will be updated internally, however you must increment it by the amount you
     * have consumed before calling again. See below.
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
     * @brief Prevents saving logging data in the circular buffer
     * 
     * This is done to make sure that if any code generates a log message while you are
     * trying to output a log message from the server, it's ignored. Otherwise, you could
     * get into an infinite recursion of log messages. Usually you recursionLock() right
     * before readNoCopy() or readLines() and recursionUnlock() after you've written the
     * data.
     */
    void recursionLock() { recursionCount++; };

    /**
     * @brief Releases the lock from recursionLock()
     */
    void recursionUnlock() { recursionCount--; };

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
    uint8_t *buf;
    size_t bufLen;
    std::vector<RemoteLogServer *>servers;
    os_mutex_t mutex = 0;
    int recursionCount = 0;
    static RemoteLog *instance;
};

#if Wiring_WiFi

class RemoteLogTCPServer; // Forward declaration

class RemoteLogTCPSession {
public:
    RemoteLogTCPSession(RemoteLogTCPServer *server, TCPClient client);
    virtual ~RemoteLogTCPSession();

    void loop();

    bool isDone();


protected:
    RemoteLogTCPServer *server;
    TCPClient client;  
    int id = 0; 
    size_t readIndex = 0;
    uint64_t recvTime = 0;
    static int lastId;
};

/**
 * @brief Server class for sending out log messages from a TCP server (Wi-Fi only)
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
     * @brief Handle a server operation
     */
    virtual void operation(OperationCode operationCode, void *data);

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
     * @param timeout Timeout as a chrono literal. You can pass in values like 30s (30 seconds) ot 5min (5 minutes).
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

protected:
    uint16_t port;
    size_t maxConn;
    TCPServer server;
    std::vector<RemoteLogTCPSession *>sessions;
    unsigned long sessionTimeout = 0;
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
     * @brief Handle a server operation
     */
    virtual void operation(OperationCode operationCode, void *data);

protected:
    IPAddress multicastAddr;
    uint16_t port;
    UDP udp;
    bool wifiReady = false;
    uint8_t *buf;
    size_t bufLen;
    size_t readIndex = 0;
};

#endif // Wiring_WiFi

/**
 * @brief Structure stored in retained memory when using RemoteLogEventServer
 * 
 * If you don't have enough retained memory available you could store this in
 * regular RAM, however the RemoteLog buffer should also be stored in regular RAM.
 * If you store the RemoteLog buffer in retained memory and RemoteLogEventRetained
 * in regular memory, then historical events will be resent on reboot.
 */
typedef struct { // 16 bytes
    uint32_t    magic;
    uint8_t     version;
    uint8_t     structSize;
    uint16_t    reserved1;
    size_t      readIndex;
    uint32_t    reserved2;
} RemoteLogEventRetained;

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
     * @param retainedData A pointer to a RemoteLogEventRetained struct 
     * 
     * @param eventName The event name to use for the publish
     */
    RemoteLogEventServer(RemoteLogEventRetained *retainedData, const char *eventName);

    /**
     * @brief Destructor. 
     * 
     * This object is not typically deleted as you can't unregister a server, so deleting
     * it would cause a dangling pointer.
     */
    virtual ~RemoteLogEventServer();

    /**
     * @brief Handle a server operation
     */
    virtual void operation(OperationCode operationCode, void *data);

    static const uint32_t RETAINED_MAGIC = 0xd5a58e95;

protected:
    void stateWaitForMessage();
    void stateTryPublish();
    void stateFutureWait();

    RemoteLogEventRetained *retainedData;
    String eventName;
    char buf[particle::protocol::MAX_EVENT_DATA_LENGTH + 1]; // 622 bytes + null terminator
    unsigned long lastPublish = 0;
    particle::Future<bool> publishFuture;
    std::function<void(RemoteLogEventServer&)> stateHandler = &RemoteLogEventServer::stateWaitForMessage;
};


#endif /* __REMOTELOGRK_H */
