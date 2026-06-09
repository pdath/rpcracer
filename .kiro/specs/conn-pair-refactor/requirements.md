# Requirements Document

## Introduction

This specification defines the "connection pair" refactoring of upstream RPC connection management in rpcrace. The current design uses exponential backoff, a CONN_DEAD state, a RACE_RETRY_WAIT state machine, and synthetic RPC error generation to handle Bitcoin node connection drops and reconnection gaps. This refactor replaces all of that complexity with a simpler abstraction: each upstream node maintains a primary and standby connection, rotating on a fixed 10-second cycle to keep the primary connection always fresh (under the ~30s bitcoind idle timeout). The proxy layer interacts with the connection pair through a narrow interface and never sees dual-connection internals.

## Glossary

- **Conn_Pair**: A module managing two upstream TCP connections (primary and standby) to a single Bitcoin node, encapsulating rotation, swap, and availability logic.
- **Primary_Connection**: The connection within a Conn_Pair that is currently used for sending RPC requests. It is always the most recently established connection after a swap.
- **Standby_Connection**: The connection within a Conn_Pair held in reserve. It is connected (or connecting) and ready to become the primary on the next rotation or error-triggered swap.
- **Rotation**: The periodic process (every 10 seconds) of closing the old standby, opening a new connection as standby, then swapping so the newest connection becomes primary.
- **Swap**: The act of exchanging the primary and standby connection roles within a Conn_Pair.
- **Node_Available**: A Conn_Pair state indicating the primary connection is in CONN_CONNECTED state and ready to accept RPC requests.
- **Proxy**: The rpc_proxy module that handles client connections, request parsing, race dispatch, and response routing.
- **Rotation_Timer**: A 10-second periodic timer per Conn_Pair that triggers rotation when the node is idle.
- **Idle**: A Conn_Pair state where no RPC request is in progress on that node (primary is CONN_CONNECTED, not CONN_SENDING or CONN_RECEIVING).

## Requirements

### Requirement 1: Conn_Pair Module Interface

**User Story:** As a developer, I want the connection pair logic encapsulated in a separate module with a narrow interface, so that the proxy layer does not deal with dual-connection internals.

#### Acceptance Criteria

1. THE Conn_Pair module SHALL expose a `conn_pair_get_active` function that returns a pointer to the upstream_conn_t currently designated as active, or NULL if neither connection is in CONN_CONNECTED state.
2. THE Conn_Pair module SHALL expose a `conn_pair_is_available` function that returns true if and only if the active connection is in CONN_CONNECTED state, and false otherwise.
3. WHEN the proxy calls `conn_pair_report_error`, THE Conn_Pair module SHALL promote the standby connection to active and demote the current active to standby, regardless of the standby connection's current state.
4. WHEN the proxy calls `conn_pair_tick`, THE Conn_Pair module SHALL evaluate whether a periodic rotation is due (based on a configured rotation interval) and, if so, swap the active and standby connections and initiate reconnection of the newly-demoted standby.
5. THE Conn_Pair module SHALL expose `conn_pair_init` and `conn_pair_destroy` functions that allocate and free both upstream_conn_t connections and the rotation timer.
6. THE Proxy SHALL use the Conn_Pair interface exclusively for connection management operations (availability checking, error reporting, rotation), while retaining direct access to the upstream_conn_t pointer returned by `conn_pair_get_active` for send and recv operations.

### Requirement 2: Connection Pair Startup

**User Story:** As a system operator, I want rpcrace to establish both primary and standby connections at startup, so that rotation can begin immediately.

#### Acceptance Criteria

1. WHEN rpcrace starts, THE Conn_Pair module SHALL initiate a non-blocking connect (via rpc_conn_connect) for both the primary connection and the standby connection to the configured upstream node, with the primary connection designated as the active connection for serving requests.
2. WHEN rpcrace starts, THE Conn_Pair module SHALL arm the rotation timer with a 10-second recurring interval using the event loop timer facility.
3. IF either connection fails to establish at startup, THEN THE Conn_Pair module SHALL continue attempting to connect in the background using the rotation timer cycle.
4. IF conn_pair_init fails to allocate resources or initialize either upstream_conn_t (rpc_conn_init returns -1), THEN THE Conn_Pair module SHALL return -1 to the caller and release any partially-allocated resources.

### Requirement 3: Timer-Driven Rotation

**User Story:** As a system operator, I want connections rotated every 10 seconds, so that the primary connection is always fresh and avoids the ~30s bitcoind idle timeout.

#### Acceptance Criteria

1. WHEN the 10-second rotation timer fires, THE Conn_Pair module SHALL set the swap_required flag and re-arm the 10-second timer regardless of whether rotation executes immediately.
2. IF swap_required is true AND the node is idle AND the standby connection is in CONN_CONNECTED state, THEN THE Conn_Pair module SHALL execute the rotation sequence.
3. IF swap_required is true AND the node is not idle (primary is in CONN_SENDING or CONN_RECEIVING state), THEN THE Conn_Pair module SHALL defer the rotation until the next timer tick or until the request completes and conditions are re-evaluated.
4. WHEN the rotation sequence executes, THE Conn_Pair module SHALL close the current standby connection and release its file descriptor.
5. WHEN the rotation sequence executes, THE Conn_Pair module SHALL open a new non-blocking connection and assign it as the standby.
6. IF the new standby connection reaches CONN_CONNECTED state, THEN THE Conn_Pair module SHALL swap the new standby to primary and the old primary to standby and clear the swap_required flag.
7. IF the new standby connection fails to establish (remains in CONN_CONNECTING or transitions to CONN_DISCONNECTED), THEN THE Conn_Pair module SHALL retain the current primary as-is, keep swap_required set, and reattempt on the next timer tick.
8. THE Conn_Pair module SHALL ensure the primary connection is always the most recently established connection after a successful rotation completes.

### Requirement 4: Error-Triggered Swap

**User Story:** As a system operator, I want a failed mid-request connection to trigger an immediate swap to the standby, so that the request can be retried without waiting for the next rotation cycle.

#### Acceptance Criteria

1. WHEN the proxy reports an error on the primary connection while the primary is in CONN_SENDING or CONN_RECEIVING state, THE Conn_Pair module SHALL swap the standby connection to primary regardless of the standby connection's current state.
2. WHEN an error-triggered swap occurs AND the new primary (former standby) is in CONN_CONNECTED state, THE Conn_Pair module SHALL return an availability indicator signaling the node is available for retry.
3. WHEN an error-triggered swap occurs AND the new primary is not in CONN_CONNECTED state, THE Conn_Pair module SHALL return an availability indicator signaling the node is unavailable.
4. WHEN an error-triggered swap occurs, THE Conn_Pair module SHALL close the old primary's file descriptor, set its state to CONN_DISCONNECTED, and clear its send and receive buffer state so that the next rotation cycle can reconnect it.
5. WHEN an error-triggered swap occurs, THE Conn_Pair module SHALL reset the rotation timer to 10 seconds from the swap event so the old primary (now standby) is eligible for reconnection on the next cycle.

### Requirement 5: Single Retry on Error

**User Story:** As a system operator, I want a single retry attempt on error (swap + retry), so that transient connection drops are handled without complex retry state machines.

#### Acceptance Criteria

1. WHEN an RPC request encounters a transport-level failure on the primary connection (connection reset, send error, or receive error), THE Proxy SHALL report the error to the Conn_Pair and request a swap.
2. WHEN the swap produces an available node (new primary in CONN_CONNECTED state), THE Proxy SHALL retry the original request exactly once on the new primary using the same client_recv_buf contents.
3. IF the swap reports the node as unavailable (standby not in CONN_CONNECTED state), THEN THE Proxy SHALL skip retry for that node and treat it as failed for the current request.
4. IF the retry also fails on the new primary (double failure), THEN THE Proxy SHALL mark that Conn_Pair as unavailable until the next rotation cycle reconnects it.
5. WHILE a fan-out race is active AND a node experiences a double failure, THE Proxy SHALL continue the race with remaining nodes that have not failed.
6. IF a sticky request's only node experiences a double failure, THEN THE Proxy SHALL close the client connection.
7. THE Proxy SHALL NOT implement exponential backoff, retry polling, or retry deadline timers for error recovery.

### Requirement 6: Both Connections Down

**User Story:** As a system operator, I want rpcrace to handle the case where both connections to a node are down, so that the rotation timer can eventually recover the node.

#### Acceptance Criteria

1. WHILE both primary and standby connections are in a non-usable state (CONN_DISCONNECTED or CONN_CONNECTING), THE Conn_Pair module SHALL report the node as unavailable.
2. WHILE the node is unavailable, THE Conn_Pair module SHALL continue firing the 10-second rotation timer and on each fire SHALL attempt to open a new connection in the standby slot.
3. WHEN the standby connection transitions to CONN_CONNECTED while the node is unavailable, THE Conn_Pair module SHALL swap it to primary and report the node as available.
4. IF a reconnection attempt initiated by the rotation timer fails, THEN THE Conn_Pair module SHALL leave the node unavailable and retry on the next 10-second timer fire without increasing the interval.
5. THE Conn_Pair module SHALL NOT transition a connection to a permanent dead state; a node that is unavailable remains eligible for reconnection indefinitely.

### Requirement 7: All Nodes Unavailable

**User Story:** As a system operator, I want rpcrace to close the client connection when all upstream nodes are unavailable, so that the downstream stratum proxy handles reconnection and retry.

#### Acceptance Criteria

1. WHEN an RPC request arrives AND no upstream Conn_Pair is in CONN_CONNECTED state at dispatch time, THE Proxy SHALL close the client TCP connection without writing any response bytes.
2. WHEN a race or broadcast dispatch completes AND all upstream responses are errors or timeouts AND no successful response has been forwarded, THE Proxy SHALL close the client TCP connection without writing any response bytes.
3. THE Proxy SHALL NOT generate synthetic JSON-RPC error responses for unreachable upstream nodes.
4. THE Proxy SHALL NOT implement the RACE_RETRY_WAIT state machine for waiting on node availability.
5. WHEN the sticky node is unreachable AND fan-out fallback finds no CONN_CONNECTED upstream nodes, THE Proxy SHALL close the client TCP connection without writing any response bytes.

### Requirement 8: Broadcast Methods

**User Story:** As a system operator, I want broadcast methods (submitblock, sendrawtransaction) sent to each available node's primary connection with a swap-and-retry on transport failure, so that blocks and transactions propagate to as many nodes as possible.

#### Acceptance Criteria

1. WHEN a broadcast method is dispatched, THE Proxy SHALL send the request to each Conn_Pair's primary connection where the node is available.
2. WHEN a broadcast method is dispatched AND a node's primary connection is not in CONN_CONNECTED state, THE Proxy SHALL skip that node.
3. WHEN a broadcast request encounters a transport-level failure on a node's primary connection, THE Proxy SHALL report the error to that node's Conn_Pair, triggering a swap, and retry the broadcast on the new primary if available.
4. IF the retry also fails on the new primary (double failure), THEN THE Proxy SHALL treat that node as unavailable for this broadcast without further retry.
5. WHEN a broadcast method is dispatched, THE Proxy SHALL allow all in-flight broadcast requests to complete on every targeted node without aborting any request.
6. WHEN the first targeted node responds to a broadcast with a successful RPC result, THE Proxy SHALL return that success response to the client.
7. IF all targeted nodes respond to a broadcast with HTTP or RPC errors, THEN THE Proxy SHALL return the last error received to the client.

### Requirement 9: Swap Preconditions

**User Story:** As a developer, I want swaps to execute only when safe, so that in-flight requests are not corrupted by a rotation.

#### Acceptance Criteria

1. WHILE the primary connection is in CONN_SENDING or CONN_RECEIVING state, THE Conn_Pair module SHALL NOT execute a timer-driven rotation on that node.
2. WHILE the standby connection is not in CONN_CONNECTED state, THE Conn_Pair module SHALL NOT execute a timer-driven rotation on that node.
3. WHEN an error-triggered swap occurs, THE Conn_Pair module SHALL execute the swap regardless of the standby connection state.
4. WHEN a timer-driven rotation is blocked by criterion 1 or criterion 2, THE Conn_Pair module SHALL retain the swap_required flag so that rotation executes on the next tick where both preconditions are satisfied.

### Requirement 10: Test Failure Review

**User Story:** As a developer, I want to review any test failures caused by this refactor before tests are rewritten, removed, or code is changed to make them pass, so that I can decide which edge cases are worth preserving and which can be dropped for simplicity.

#### Acceptance Criteria

1. WHEN the refactor causes existing unit tests to fail, THE developer SHALL present the list of failing tests and their failure reasons to the operator for review before taking corrective action.
2. THE developer SHALL NOT rewrite, remove, or modify failing tests without explicit operator approval for each test.
3. THE developer SHALL NOT modify implementation code solely to make a failing edge-case test pass without explicit operator approval.
4. WHEN the operator approves removal of a test, THE developer SHALL remove it and document the rationale.
5. WHEN the operator requests a test be preserved, THE developer SHALL update the implementation or test to ensure it passes.

### Requirement 11: Code Removal

**User Story:** As a developer, I want obsolete reconnection and retry code removed, so that the codebase is simpler and the old complexity does not interact with the new design.

#### Acceptance Criteria

1. THE codebase SHALL NOT contain the CONN_DEAD state or the RECONNECT_DEAD_THRESHOLD constant.
2. THE codebase SHALL NOT contain the rpc_conn_schedule_reconnect function, the RECONNECT_MAX_MS constant, or the exponential backoff delay loop that doubles reconnect_base_ms per attempt.
3. THE codebase SHALL NOT contain the rpc_conn_try_reconnect function or periodic reconnect polling.
4. THE codebase SHALL NOT contain the reconnect_attempts, next_reconnect_ns, or reconnect_base_ms fields in the upstream connection structure.
5. THE codebase SHALL NOT contain the RACE_RETRY_WAIT state, enter_retry_wait, retry_poll_cb, or retry_deadline_cb functions.
6. THE codebase SHALL NOT contain the send_rpc_error_to_client function.
7. WHEN rpc_conn_reset is called and the connection_close flag is set, THE system SHALL disconnect and transition to CONN_DISCONNECTED without scheduling a reconnect timer (rotation handles idle connection refresh externally).
8. WHEN code removal is complete, THE codebase SHALL compile without errors and all remaining tests SHALL pass without referencing any of the removed identifiers listed in criteria 1 through 6.
9. THE codebase SHALL NOT contain the retry_timer_fd, retry_deadline_timer_fd, retry_attempts, or is_post_notify_gbt fields in the proxy structure.

### Requirement 12: Preserved Functionality

**User Story:** As a system operator, I want existing race logic, HTTP framing, JSON-RPC parsing, and event loop machinery preserved, so that the refactor only changes connection management.

#### Acceptance Criteria

1. THE Proxy SHALL continue to support fan-out race (ROUTE_RACE), sticky (ROUTE_STICKY), and broadcast (ROUTE_BROADCAST) routing strategies, where method classification maps getblocktemplate to race or sticky, submitblock and sendrawtransaction to broadcast, preciousblock to sticky, and all other methods to fan-out race.
2. WHEN a getblocktemplate fan-out race receives multiple success responses, THE Proxy SHALL select the winner by comparing the integer "height" field in each response and choosing the response with the strictly greatest height value.
3. THE Proxy SHALL continue to parse HTTP/1.1 request framing by locating the "\r\n\r\n" header terminator and extracting the Content-Length header value to determine body boundaries.
4. THE Proxy SHALL continue to extract the JSON-RPC "method" field from the request body using yyjson and store it in a buffer of 128 bytes maximum length.
5. THE system SHALL continue to use the epoll-based event loop (epoll_wait dispatch, timerfd timers, EPOLLIN/EPOLLOUT/EPOLLET event masks) for all I/O including listener, client, upstream, and timer file descriptors.
6. WHEN an upstream socket is created, THE system SHALL configure it with TCP_NODELAY, SO_KEEPALIVE, and SO_SNDBUF/SO_RCVBUF set to SOCKET_BUF_SIZE via the configure_socket helper.

### Requirement 13: Resource Limits

**User Story:** As a system operator, I want the connection pair design to work within expected resource limits, so that file descriptor usage remains bounded.

#### Acceptance Criteria

1. THE system SHALL support up to 16 upstream nodes with 2 connections per node and 1 rotation timerfd per node, consuming at most 48 upstream file descriptors (3 per node × 16 nodes) when all nodes are configured.
2. THE Conn_Pair module SHALL use a fixed 10-second rotation interval that is hardcoded, not exposed in configuration, and not adaptive, ensuring the standby connection is always active well within the 30-second bitcoind idle-disconnect window.
3. THE system SHALL consume no more than 55 file descriptors in total under maximum configuration (48 upstream + 1 listener + 1 client + 1 epoll + up to 4 one-shot timer fds for RPC timeout and retry).
