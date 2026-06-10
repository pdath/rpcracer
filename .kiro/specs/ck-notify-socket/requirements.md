# Requirements Document

## Introduction

This feature adds an optional Unix domain socket notification mechanism to rpcrace's downstream relay subsystem. When a new block is accepted, rpcrace sends the "update" command to ckpool's stratifier Unix socket using ckpool's native length-prefixed protocol. This provides significantly lower latency than the existing ZMQ downstream relay (~50ms round-trip) by communicating directly with ckpool over a local Unix socket with non-blocking I/O via epoll.

## Glossary

- **Notifier**: The rpcrace subsystem responsible for receiving upstream block notifications, deduplicating them, and relaying them downstream to stratum proxies.
- **CK_Socket_Relay**: The new component within the Notifier that sends "update" messages to ckpool's stratifier Unix domain socket.
- **Stratifier_Socket**: The Unix domain socket exposed by ckpool (default path: `/tmp/ckpool/stratifier`) that accepts length-prefixed commands.
- **Length_Prefix**: A 32-bit little-endian unsigned integer prepended to the message payload, representing the byte length of the payload that follows.
- **Event_Loop**: The epoll-based event loop that manages all file descriptors and timers in rpcrace.
- **Config_Parser**: The JSON configuration parser that reads `rpcrace.conf` and populates the `config_t` structure.

## Requirements

### Requirement 1: Configuration Option

**User Story:** As an operator, I want to configure the ckpool stratifier socket path in `rpcrace.conf`, so that I can enable Unix socket notifications to ckpool without modifying source code.

#### Acceptance Criteria

1. WHEN the configuration file contains a `"ck_notify_socket"` field with a string value of 1 to 107 characters, THE Config_Parser SHALL store the value in the `config_t.ck_notify_socket` field (a 108-byte buffer) and `config_load` SHALL return a valid configuration that causes the CK_Socket_Relay to initialize at startup.
2. IF the configuration file does not contain the `"ck_notify_socket"` field, or the value is an empty string, THEN THE Config_Parser SHALL set `config_t.ck_notify_socket` to an empty string (first byte NUL) and the CK_Socket_Relay SHALL not initialize.
3. IF `"ck_notify_socket"` contains a string longer than 107 characters, THEN THE Config_Parser SHALL log an error message indicating the path exceeds the 107-character limit, and `config_load` SHALL return NULL.
4. IF `"ck_notify_socket"` is present but its JSON value is not a string type, THEN THE Config_Parser SHALL log an error message indicating an invalid type for the field, and `config_load` SHALL return NULL.

### Requirement 2: Connection Establishment

**User Story:** As an operator, I want rpcrace to connect to the ckpool stratifier socket at startup, so that notifications can be sent immediately when a new block arrives.

#### Acceptance Criteria

1. WHEN a non-empty Stratifier_Socket path is present in the configuration, THE CK_Socket_Relay SHALL create a Unix domain socket (AF_UNIX, SOCK_STREAM) and connect to that path during notifier initialization.
2. IF the connection to the Stratifier_Socket fails at startup (connect() returns an error), THEN THE CK_Socket_Relay SHALL log a warning indicating the failure reason and continue operation without the Unix socket relay.
3. WHEN the CK_Socket_Relay successfully connects to the Stratifier_Socket, THE CK_Socket_Relay SHALL set the socket file descriptor to non-blocking mode (O_NONBLOCK).

### Requirement 3: Message Framing

**User Story:** As a developer, I want the relay to use ckpool's native length-prefixed protocol, so that the stratifier correctly parses incoming commands.

#### Acceptance Criteria

1. WHEN a block notification is accepted by the Notifier, THE CK_Socket_Relay SHALL write a contiguous 10-byte buffer containing the Length_Prefix (bytes 0x06 0x00 0x00 0x00) followed by the 6-byte ASCII payload "update" (no null terminator) in a single write() call.
2. THE CK_Socket_Relay SHALL send exactly 10 bytes per notification (4-byte little-endian length prefix encoding the value 6, immediately followed by the 6-byte payload), with no additional bytes before, between, or after the two fields.

### Requirement 4: Non-Blocking Send via epoll

**User Story:** As a developer, I want the Unix socket send to use non-blocking I/O integrated with the event loop, so that a blocked or slow ckpool socket does not stall the main event loop.

#### Acceptance Criteria

1. WHEN a block notification is accepted by the Notifier, THE CK_Socket_Relay SHALL attempt exactly one non-blocking write() call of the 10-byte message to the Stratifier_Socket, with no retry loop or subsequent write attempts for the same notification.
2. WHEN the non-blocking write completes successfully (10 bytes written), THE CK_Socket_Relay SHALL log the relay event at INFO verbosity.
3. WHEN the non-blocking write returns EAGAIN or EWOULDBLOCK, THE CK_Socket_Relay SHALL discard the notification without buffering or retrying, and log a warning.
4. WHEN the non-blocking write returns a different error (e.g., EPIPE, ECONNRESET), THE CK_Socket_Relay SHALL close the socket, log a warning, and attempt reconnection on the next notification.
5. WHEN a partial write occurs (fewer than 10 bytes written), THE CK_Socket_Relay SHALL close the socket, log a warning, and attempt reconnection on the next notification.

### Requirement 5: Reconnection

**User Story:** As an operator, I want rpcrace to automatically reconnect to the ckpool socket if the connection is lost, so that notifications resume without manual intervention.

#### Acceptance Criteria

1. WHEN the CK_Socket_Relay detects a broken connection (write error or partial write), THE CK_Socket_Relay SHALL close the existing socket file descriptor and mark the state as disconnected.
2. WHEN a new block notification arrives and the CK_Socket_Relay is in the disconnected state (whether due to a broken connection or a failed initial connection at startup), THE CK_Socket_Relay SHALL create a new Unix domain socket (AF_UNIX, SOCK_STREAM), connect to the configured Stratifier_Socket path, and set the new file descriptor to non-blocking mode (O_NONBLOCK).
3. IF reconnection fails (socket creation fails or connect() returns an error), THEN THE CK_Socket_Relay SHALL log a warning including the error description and drop the current notification.
4. WHEN reconnection succeeds, THE CK_Socket_Relay SHALL proceed to send the 10-byte notification message on the new connection using the non-blocking write path.

### Requirement 6: Integration with Existing Relay Pipeline

**User Story:** As a developer, I want the Unix socket relay to operate alongside the existing ZMQ PUB and HTTP downstream relays, so that operators can use any combination of downstream notification methods.

#### Acceptance Criteria

1. WHEN a block notification is accepted by the Notifier, THE Notifier SHALL relay the accepted block hash to the CK_Socket_Relay in the same execution path as the existing ZMQ PUB relay and HTTP notify relay.
2. IF the CK_Socket_Relay encounters a write error or connection failure, THEN THE Notifier SHALL log a warning and continue processing without affecting the ZMQ PUB relay or HTTP notify relay invocations.
3. IF the ZMQ PUB relay or HTTP notify relay encounters a failure, THEN THE Notifier SHALL still invoke the CK_Socket_Relay for the same block hash.
4. IF `ck_notify_socket` is not configured, THEN THE Notifier SHALL skip the CK_Socket_Relay invocation without logging an error.
5. WHEN neither `zmq_server_port`, `notify_http_url`, nor `ck_notify_socket` is configured, THE Notifier SHALL log a warning at startup indicating no downstream notification method is configured.

### Requirement 7: Clean Shutdown

**User Story:** As an operator, I want rpcrace to cleanly close the Unix socket on shutdown, so that ckpool does not observe a connection reset.

#### Acceptance Criteria

1. WHEN rpcrace shuts down (SIGTERM or SIGINT), THE CK_Socket_Relay SHALL close the Unix domain socket file descriptor by calling close() on the connected fd before the notifier_destroy function frees the notifier resources.
2. IF the CK_Socket_Relay is in the disconnected state (fd is -1) at shutdown time, THEN THE CK_Socket_Relay SHALL skip the close operation without error.
3. IF the CK_Socket_Relay is disabled (not configured), THEN THE CK_Socket_Relay SHALL perform no shutdown action for the Unix domain socket.

### Requirement 8: Logging and Observability

**User Story:** As an operator, I want clear log messages about the Unix socket relay status, so that I can diagnose connectivity issues.

#### Acceptance Criteria

1. WHEN the CK_Socket_Relay successfully connects to the Stratifier_Socket (either at startup or during reconnection), THE CK_Socket_Relay SHALL log an INFO message including the source tag `[ck]` and the socket path.
2. WHEN the CK_Socket_Relay successfully sends a notification, THE CK_Socket_Relay SHALL log an INFO message including the source tag `[ck]` and the block hash as a 64-character hex string.
3. WHEN the CK_Socket_Relay encounters a connection or write error, THE CK_Socket_Relay SHALL log a WARN message including the source tag `[ck]`, the socket path, and the error description from strerror.
4. WHEN the CK_Socket_Relay is disabled (not configured), THE CK_Socket_Relay SHALL produce no log output.
