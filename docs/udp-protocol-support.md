# UDP Protocol Support

Shield framework now supports UDP protocol alongside the existing TCP, HTTP, and WebSocket protocols.

## Overview

The UDP integration provides:
- **UDP Session Management**: Automatic session creation and timeout handling for connectionless UDP
- **Reactor Pattern Integration**: UDP server using the same reactor pattern as TCP components  
- **Protocol Handler**: Consistent interface with other protocol handlers
- **Prometheus Metrics**: Built-in monitoring for UDP connections, packets, and performance

## Architecture

### Core Components

- `UdpSession`: Manages UDP endpoints and session lifecycle
- `UdpProtocolHandler`: Implements the protocol handler interface for UDP
- `UdpReactor`: Multi-threaded UDP server using Boost.Asio
- `NetworkMetricsCollector`: Extended with UDP-specific metrics

### Key Features

1. **Session Management**: UDP endpoints are tracked as sessions with configurable timeouts
2. **Thread Safety**: Multi-threaded reactor pattern with worker threads
3. **Metrics Integration**: Automatic collection of UDP-specific metrics
4. **Error Handling**: Robust error handling and connection lifecycle management

## Usage Example

```cpp
#include "shield/net/udp_reactor.hpp"
#include "shield/protocol/udp_protocol_handler.hpp"

// Create UDP reactor
shield::net::UdpReactor reactor(12345, 2); // port 12345, 2 worker threads

// Set custom handler 
reactor.set_handler_creator([](boost::asio::io_context& io_context, uint16_t port) {
    return std::make_unique<MyUdpHandler>(io_context, port);
});

// Start the reactor
reactor.start();

// ... server runs until stop() is called
reactor.stop();
```

## Configuration

UDP sessions support the following configuration options:

- **Session Timeout**: How long to keep inactive UDP sessions (default: 5 minutes)
- **Cleanup Interval**: How often to check for expired sessions (default: 1 minute)
- **Worker Threads**: Number of threads for processing UDP packets

## Prometheus Metrics

The following UDP-specific metrics are automatically collected:

- `shield_active_udp_sessions`: Number of active UDP sessions  
- `shield_udp_packets_sent_total`: Total UDP packets sent
- `shield_udp_packets_received_total`: Total UDP packets received
- `shield_udp_bytes_sent_total`: Total UDP bytes sent
- `shield_udp_bytes_received_total`: Total UDP bytes received
- `shield_udp_timeouts_total`: Total UDP session timeouts

## Integration with Existing Systems

UDP protocol handlers integrate seamlessly with:
- CAF actor system for message processing
- Lua scripting engine for business logic
- Service discovery mechanisms
- Configuration management system
- Logging framework