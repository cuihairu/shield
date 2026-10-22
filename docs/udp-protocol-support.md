# UDP Support

UDP is one of the target capabilities of the refactored `net` module, but this document no longer describes the metrics, monitoring, or standalone protocol layer design from the old architecture.

## Target Boundaries

`net` is responsible for:

- UDP socket lifecycle.
- Packet send/receive.
- Session identification policy.
- Timeouts and cleanup.
- Handing raw data to transport or the Lua gateway.

`net` is NOT responsible for:

- Prometheus metrics.
- Service discovery.
- Application-level reliable delivery policy.
- Framework-level middleware.

## Transport

If a project needs reliable UDP, encryption, compression, or a private packet format, it should be implemented through the C++ `transport` extension point.

```text
UDP packet → net → transport → Lua gateway
```

## To Be Added Later

- Session key design.
- Packet size limits.
- Packet loss and replay policy.
- Message format between transport and the Lua gateway.
- Corresponding unit tests and stress tests.
