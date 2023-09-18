# RemoteLogRK

*Remote logging for Particle devices*

- Repository: https://github.com/rickkas7/RemoteLogRK
- [Full browsable API](https://rickkas7.github.io/RemoteLogRK/index.html)
- License: MIT (can be used in open or closed-source projects, including commercial projects, with no attribution required)
- Library: RemoteLogRK


This library provides several mechanisms for getting debugging logs (Log.info, Log.error, etc.) off the device:

- Syslog UDP logging (cellular or Wi-Fi) sends the logs to a syslog server. This is ideal for using Solarwinds Papertrail.
- Event logging (cellular or Wi-Fi) sends the logs out via Particle events.
- Multicast UDP logging (Wi-Fi only) sends the logs out onto the local network where they are picked by by a multicast receiver.
- TCP server logging (Wi-Fi only) provides recent logs when you connect over the network to the on-device server, along with live tail. 

A configurable buffer, which can be in regular or retained memory, can be used to hold log messages when not connected to the network, including the messages that are generating during cellular and cloud connection. When in retained memory, it's preserved across reset as well. A buffer of 2500 bytes is usually sufficient to log the messages during cloud connection, though you can make it larger if you have available RAM.

While you can enable more than one server, you will probably only have one in most cases. 

## Logging Methods

### Syslog UDP

[Syslog](https://en.wikipedia.org/wiki/Syslog), System Logging Protocol, is a standard for transmitting system logs over a network. For Wi-Fi devices you can transmit to a server on your local network, such as a Linux server. Or you can use a cloud service. [Solarwinds Papertrail](https://www.papertrail.com/) is a popular one. 

The nice thing about using a service like Papertrail is that the take care of storing and have a nice interface for live tail and searching the logs. The downside is that if you have anything more than the bare minimum of logs you'll need to pay for it, and when using UDP, the logs are not encrypted in transit, so you should avoid logging anything sensitive. 

Papertrail supports TLS/SSL encrypted syslog over TCP, but this library does not as the TLS library tends to use up most of the available flash on devices making it impractical.


### Event 

Event based logging buffers the logs in retained memory or regular memory, and then transits them once cloud connected. Multiple log messages can be combined into a single event, and are automatically rate limited to the one event per second rate limit.

The idea is that you'd collect these logs using Server Sent Events (SSE) or Webhooks and store them somewhere on your own server, or store them in a cloud-based database.

The advantage of events is that they are secure and take advantage of the Particle cloud encryption layer so there is little extra code or handshaking required. And event-based logging works on both cellular and Wi-Fi.

### Multicast UDP

The Multicast UDP option is available for Wi-Fi devices. 

Pros:
- Small code
- Efficient
- Does not require a server with a fixed IP address

Cons:
- Not authenticated or encrypted
- Anyone on the LAN can monitor all logs in real time


### TCP server

The TCP server option is available for Wi-Fi devices. It works the opposite of the other logging methods. The recent logs are accumulated locally, and when you connect to the TCP server the recent logs are returned, then the connection has a live tail of new logging entries. This is useful if you don't normally care about the logs, but if there's a problem you want to be able to connect to the device over the local LAN.

The disadvantage is that the transport is not encrypted, there is no authentication to connect to the server, and when using DHCP it's hard to know the IP address of the device you want to connect to. But it may be useful in some cases.


## Version History

### 0.0.4

- Switch to using Network.resolve() which was added in Device OS 1.1.0.

### 0.0.3

- Documentation
- Added ability to set syslog host and port after construction

### 0.0.2 (2021-02-15)

- Added Papertrail (syslog UDP) logging
- Changed retained memory structure to make it easier to implement readIndex in server handlers

### 0.0.1 (2021-01-19)

- Initial version

