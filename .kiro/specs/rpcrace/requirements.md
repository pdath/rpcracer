# Requirements Document

## Introduction

rpcrace is a high-performance RPC proxy and block notification relay written in C that sits between Bitcoin stratum proxies (such as ckpool or DATUM Gateway) and an array of upstream Bitcoin nodes. It races RPC requests across multiple geographically distributed nodes to minimize latency for time-critical operations like `getblocktemplate()` and `submitblock()`, reducing the impact of lock contention and variable response times inherent to individual Bitcoin nodes.

## Glossary

- **RPC_Proxy**: The rpcrace component that receives JSON-RPC requests from stratum proxies and forwards them concurrently to the upstream Bitcoin node array, returning the fastest valid response.
- **Notifier_Proxy**: The rpcrace component that receives block notifications from Bitcoin nodes (via ZMQ or HTTP) and relays them to stratum proxies.
- **Bitcoin_Node**: An upstream Bitcoin Core or Bitcoin Knots instance configured as part of the node array.
- **Stratum_Proxy**: A downstream client (such as ckpool or DATUM Gateway) that connects to rpcrace for RPC services and block notifications.
- **Node_Array**: The configured set of upstream Bitcoin nodes that rpcrace races requests against.
- **Sticky_Node**: The Bitcoin node selected to handle subsequent getblocktemplate() requests after the initial race following a block notification.
- **Block_Notify**: A notification event indicating a new block has been added to the Bitcoin blockchain, identified by a 32-byte block hash.
- **Config_File**: A JSON configuration file (`rpcrace.conf`) specifying the node array, ZMQ endpoints, listener addresses, and operational parameters.
- **Statistics_Collector**: The component that gathers performance metrics about upstream node responses in a non-blocking fashion.

## Requirements

### Requirement 1: Block Notification Reception via ZMQ

**User Story:** As a stratum proxy operator, I optionally want rpcrace to subscribe to ZMQ hashblock messages from each configured Bitcoin node, so that new block events are detected with minimal latency.

#### Acceptance Criteria

1. THE Notifier_Proxy SHALL subscribe to the ZMQ "hashblock" topic on each Bitcoin_Node configured with a ZMQ endpoint.
2. WHEN a Bitcoin_Node publishes a ZMQ "hashblock" message, THE Notifier_Proxy SHALL receive and process the message.
3. WHEN a subsequent block notification is received from any Bitcoin_Node, via either ZMQ or HTTP, that has the same block hash as the prior notification, THE Notifier_Proxy SHALL suppress the duplicate without further processing.
4. IF a ZMQ TCP connection to a Bitcoin_Node fails unexpectedly, THEN THE Notifier_Proxy SHALL attempt to reconnect and resume subscription without operator intervention.
5. IF a Bitcoin_Node restarts, THEN THE Notifier_Proxy SHALL detect the disconnection and re-establish the ZMQ subscription.

### Requirement 2: Block Notification Reception via HTTP

**User Story:** As a stratum proxy operator, I optionally want rpcrace to accept HTTP block notifications from Bitcoin nodes using the blocknotify mechanism, so that nodes without ZMQ can still trigger new block events.

Example Bitcoin node configuration that triggers this endpoint:

`blocknotify=wget -q -O /dev/null http://192.168.10.12:7152/NOTIFY/%s`

#### Acceptance Criteria

1. WHEN an HTTP GET request is received at the path `/NOTIFY/<blockhash>`, THE Notifier_Proxy SHALL process the block hash as a new block notification.
2. WHEN a subsequent block notification is received from any Bitcoin_Node, via either ZMQ or HTTP, that has the same block hash as the prior notification, THE Notifier_Proxy SHALL suppress the duplicate without further processing.
3. THE Notifier_Proxy SHALL respond to valid HTTP notify requests with an HTTP 200 status code.
4. IF an HTTP notify request is received with an invalid or missing block hash, THEN THE Notifier_Proxy SHALL respond with an HTTP 400 status code.

### Requirement 3: Block Notification Relay to Stratum Proxies

**User Story:** As a stratum proxy operator, I want rpcrace to notify my stratum proxy of new blocks, so that it can immediately request a fresh block template.

#### Acceptance Criteria

1. WHEN a new unique block hash is received from any Bitcoin_Node, THE Notifier_Proxy SHALL relay the block notification to the configured Stratum_Proxy via ZMQ or an HTTP notify request.
2. WHERE ZMQ relay is configured, THE Notifier_Proxy SHALL publish the block hash on the "hashblock" ZMQ topic.
3. WHERE an HTTP notify URL is configured, THE Notifier_Proxy SHALL issue a non-blocking HTTP GET request to the configured URL with the block hash substituted for the `%s` placeholder, if present.
4. WHERE an HTTP notify URL is configured without a `%s` placeholder, THE Notifier_Proxy SHALL issue the HTTP GET request to the URL as-is without substitution.
5. THE Notifier_Proxy SHALL relay only the first occurrence of each unique block hash to the Stratum_Proxy.

### Requirement 4: RPC Proxy General Request Handling

**User Story:** As a stratum proxy operator, I want rpcrace to proxy general RPC requests to the fastest responding Bitcoin node, so that my stratum proxy experiences minimal latency.

#### Acceptance Criteria

1. WHEN the RPC_Proxy receives an RPC request that is not getblocktemplate, submitblock, sendrawtransaction, preciousblock, validateaddress, or decoderawtransaction, THE RPC_Proxy SHALL forward the request concurrently to all Bitcoin nodes in the Node_Array.
2. THE RPC_Proxy SHALL allow all concurrent requests to complete on every Bitcoin_Node without aborting any in-flight request; responses received after the winning response SHALL be discarded.
3. WHEN the first Bitcoin_Node responds without an HTTP error or an RPC error, THE RPC_Proxy SHALL return that response to the Stratum_Proxy.
4. IF all Bitcoin nodes in the Node_Array respond with HTTP or RPC errors, THEN THE RPC_Proxy SHALL return the last error received to the Stratum_Proxy.
5. WHEN an RPC method name is not in the expected list of supported methods (getblocktemplate, submitblock, sendrawtransaction, preciousblock, validateaddress, decoderawtransaction), THE RPC_Proxy SHALL log the method name as unexpected and process the request using the general concurrent forwarding strategy.

### Requirement 5: getblocktemplate() Race Handling

**User Story:** As a stratum proxy operator, I want the first getblocktemplate() call after a new block notification to race across all nodes, so that I get the fastest valid template for the new block height.

#### Acceptance Criteria

1. WHEN getblocktemplate() is called for the first time after a Block_Notify event, THE RPC_Proxy SHALL forward the request concurrently to all Bitcoin nodes in the Node_Array.
2. WHEN a Bitcoin_Node responds with a valid getblocktemplate() result for the next expected block height, THE RPC_Proxy SHALL return that response to the Stratum_Proxy and designate that Bitcoin_Node as the Sticky_Node.
3. IF no Bitcoin_Node responds with the next expected block height, THEN THE RPC_Proxy SHALL select the response with the highest block height among all valid responses, return the last such response received to the Stratum_Proxy, and designate that Bitcoin_Node as the Sticky_Node.
4. WHILE a Sticky_Node is designated and no new Block_Notify event has occurred, THE RPC_Proxy SHALL forward subsequent getblocktemplate() requests exclusively to the Sticky_Node.
5. THE RPC_Proxy SHALL record the block height from a successful getblocktemplate() response only if it is greater than the previously recorded block height.
6. IF all Bitcoin nodes respond with HTTP or RPC errors during a getblocktemplate() race, THEN THE RPC_Proxy SHALL return the last error received to the Stratum_Proxy.
7. WHEN getblocktemplate() is called before any Block_Notify event has been received since startup, THE RPC_Proxy SHALL race the request across all nodes, accept the first valid response, and designate that Bitcoin_Node as the Sticky_Node.

### Requirement 6: submitblock() Broadcast Handling

**User Story:** As a stratum proxy operator, I want submitblock() to be sent to all nodes and never aborted, so that my solved block has the maximum chance of being accepted by the network.

#### Acceptance Criteria

1. WHEN a submitblock() request is received, THE RPC_Proxy SHALL forward the request concurrently to all Bitcoin nodes in the Node_Array.
2. THE RPC_Proxy SHALL allow all submitblock() requests to complete on every Bitcoin_Node without aborting any in-flight request.
3. WHEN the first Bitcoin_Node responds with a successful RPC result, THE RPC_Proxy SHALL return that success response to the Stratum_Proxy.
4. IF all Bitcoin nodes respond with HTTP or RPC errors, THEN THE RPC_Proxy SHALL return the last error received to the Stratum_Proxy.

### Requirement 7: sendrawtransaction() Broadcast Handling

**User Story:** As a stratum proxy operator, I want sendrawtransaction() to be broadcast to all nodes without aborting, so that transactions propagate reliably across the network.

#### Acceptance Criteria

1. WHEN a sendrawtransaction() request is received, THE RPC_Proxy SHALL forward the request concurrently to all Bitcoin nodes in the Node_Array.
2. THE RPC_Proxy SHALL allow all sendrawtransaction() requests to complete on every Bitcoin_Node without aborting any in-flight request.
3. WHEN the first Bitcoin_Node responds with a successful RPC result, THE RPC_Proxy SHALL return that success response to the Stratum_Proxy.
4. IF all Bitcoin nodes respond with HTTP or RPC errors, THEN THE RPC_Proxy SHALL return the last error received to the Stratum_Proxy.

### Requirement 8: preciousblock() Routing

**User Story:** As a stratum proxy operator, I want preciousblock() to be sent to the node currently providing my block templates, so that the preferred chain tip is communicated to the correct node.

#### Acceptance Criteria

1. WHEN a preciousblock() request is received, THE RPC_Proxy SHALL forward the request to the current Sticky_Node.
2. IF no Sticky_Node has been designated, THEN THE RPC_Proxy SHALL return an RPC error to the Stratum_Proxy indicating no sticky node is available.
3. IF the current Sticky_Node is unreachable, THEN THE RPC_Proxy SHALL return an RPC error to the Stratum_Proxy.

### Requirement 9: Non-Blocking Logging

**User Story:** As a system operator, I want logging to be non-blocking with microsecond timestamps, so that logging does not introduce latency and I can perform precision timing analysis.

#### Acceptance Criteria

1. THE RPC_Proxy SHALL write all log messages to stderr.
2. THE RPC_Proxy SHALL set stderr to O_NONBLOCK mode at startup.
3. THE RPC_Proxy SHALL include a microsecond-accurate timestamp in ISO 8601 format in every log message.
4. THE RPC_Proxy SHALL include the source identifier (Bitcoin_Node IP address or Stratum_Proxy IP address) in every log message.
5. THE RPC_Proxy SHALL log all Block_Notify messages sent and received, including the block hash.
6. THE RPC_Proxy SHALL log which upstream Bitcoin_Node won each RPC race.
7. THE RPC_Proxy SHALL log the RPC method name for every RPC request received.
8. WHEN a getblocktemplate() or submitblock() request completes, THE RPC_Proxy SHALL log additional detail including response time and relevant metadata.
9. WHEN an RPC response is received from a Bitcoin_Node and the elapsed time exceeds 5 seconds, THE RPC_Proxy SHALL log a warning identifying the slow node and the elapsed time.
10. IF a write to stderr would block, THEN THE RPC_Proxy SHALL discard the log message without stalling.

### Requirement 10: Performance Statistics Collection

**User Story:** As a stratum proxy operator, I want performance statistics collected per node, so that I can monitor node health and response times.

#### Acceptance Criteria

1. THE Statistics_Collector SHALL record the response time of each getblocktemplate() request per Bitcoin_Node.
2. THE Statistics_Collector SHALL record the number of transactions reported in each getblocktemplate() response per Bitcoin_Node.
3. THE Statistics_Collector SHALL record the elapsed time from Block_Notify reception to the winning getblocktemplate() response.
4. WHEN an HTTP GET request is received at the statistics endpoint on the shared HTTP listener, THE Statistics_Collector SHALL return the current statistics as a JSON response.

### Requirement 11: JSON Configuration File

**User Story:** As a system operator, I want to configure rpcrace via a JSON file, so that I can define the node array, endpoints, and operational parameters without recompilation.

#### Acceptance Criteria

1. THE RPC_Proxy SHALL read configuration from a JSON file named `rpcrace.conf` at startup.
2. WHEN a configuration file path is specified on the command line, THE RPC_Proxy SHALL use that file.
3. WHEN no configuration file path is specified on the command line, THE RPC_Proxy SHALL look for `rpcrace.conf` in the current working directory.
4. IF the configuration file cannot be read or parsed, THEN THE RPC_Proxy SHALL exit with a descriptive error message.
5. THE Config_File SHALL support specifying the Node_Array, where each node includes an RPC endpoint (host:port) and an optional ZMQ endpoint.
6. THE Config_File SHALL include a globally configured HTTP listener port used by both the block notification endpoint and the statistics endpoint.
7. THE Config_File SHALL include a configurable RPC listener bind address and port, supporting localhost-only (127.0.0.1), all interfaces (0.0.0.0), or a specific interface IP address.
8. THE Config_File SHALL include a global RPC timeout value.
9. THE Config_File SHALL include a global logging verbosity setting.
10. THE Config_File SHALL include an optional HTTP notify URL for downstream block notification relay to the Stratum_Proxy.
11. THE Config_File SHOULD document that at least one notification method is expected per Bitcoin_Node, where ZMQ is configured in `rpcrace.conf` and HTTP blocknotify is assumed to be configured externally on the Bitcoin_Node.

### Requirement 12: Network I/O and Connection Management

**User Story:** As a system operator, I want rpcrace to use non-blocking I/O with persistent connections, so that network operations are efficient and resilient.

#### Acceptance Criteria

1. THE RPC_Proxy SHALL use non-blocking network I/O via epoll for all socket operations.
2. THE RPC_Proxy SHALL maintain long-lived TCP connections to each Bitcoin_Node using TCP_NODELAY and SO_KEEPALIVE socket options.
3. THE RPC_Proxy SHALL request large send and receive socket buffers from the operating system.
4. IF the operating system returns a socket buffer size smaller than requested, THEN THE RPC_Proxy SHALL log a warning including the actual buffer size returned by getsockopt().
5. THE RPC_Proxy SHALL apply a single configurable global timeout to all RPC requests across all Bitcoin nodes.
6. IF a TCP connection to a Bitcoin_Node fails, THEN THE RPC_Proxy SHALL attempt to re-establish the connection without operator intervention.
7. THE RPC_Proxy SHALL pass through HTTP Basic Authentication credentials from the Stratum_Proxy request to the upstream Bitcoin_Node without modification or validation.
8. IF a Bitcoin_Node connection drops or produces a truncated response mid-transfer, THEN THE RPC_Proxy SHALL treat it as an error response from that node and proceed with the race logic using remaining responses.
9. THE RPC_Proxy SHALL treat the incoming RPC request buffer as read-only and send it to all upstream Bitcoin nodes without copying the payload data.
10. THE RPC_Proxy SHALL use CLOCK_MONOTONIC for all timeout calculations and elapsed time measurements to avoid sensitivity to system clock adjustments.

### Requirement 13: Client Connection Management

**User Story:** As a system operator, I want rpcrace to handle stratum proxy connections gracefully, so that client restarts and disconnects do not disrupt upstream operations.

#### Acceptance Criteria

1. THE RPC_Proxy SHALL support a single Stratum_Proxy client connection at a time. IF a new client connection is received while an existing connection is active, THEN THE RPC_Proxy SHALL drop the existing connection and accept the new one.
2. IF the Stratum_Proxy disconnects while upstream requests are in-flight, THEN THE RPC_Proxy SHALL allow those upstream requests to complete and discard the responses.
3. IF all Bitcoin nodes in the Node_Array are unreachable or fail to respond, THEN THE RPC_Proxy SHALL immediately return an error to the Stratum_Proxy and log a critical message indicating the entire node array is unavailable.

### Requirement 14: Process Lifecycle

**User Story:** As a system operator, I want rpcrace to start reliably and shut down cleanly, so that it integrates well with process supervisors and avoids data loss.

#### Acceptance Criteria

1. THE RPC_Proxy SHALL start and begin accepting client requests even if no Bitcoin nodes in the Node_Array are currently reachable, and SHALL continue attempting to establish connections in the background.
2. WHEN the RPC_Proxy receives SIGTERM or SIGINT, THE RPC_Proxy SHALL close all connections cleanly and exit.
3. IF the event loop has not processed any event (including timeouts) within a configurable stall threshold, THEN THE RPC_Proxy SHALL log a critical error and terminate the process to allow the supervisor to restart it.

### Requirement 15: Build System and Compilation

**User Story:** As a developer, I want a plain Makefile with strict compiler options, so that the project builds reliably with maximum compile-time error detection.

#### Acceptance Criteria

1. THE Build_System SHALL use a plain Makefile compatible with GNU Make.
2. THE Build_System SHALL provide a configure script that uses pkg-config to detect library paths and flags.
3. THE Build_System SHALL compile the project using gcc with strict warning and error flags enabled.
4. THE Build_System SHALL enable link-time optimizations.
5. THE Build_System SHALL enable optimizations for the native CPU architecture.
6. THE Build_System SHALL produce binaries that run on Ubuntu 24.04 and Debian 13 on both x86_64 and ARM64 architectures.

### Requirement 16: Deployment Artifacts

**User Story:** As a system operator, I want example systemd units and a Dockerfile, so that I can deploy rpcrace in production or containerized environments with minimal effort.

#### Acceptance Criteria

1. THE Project SHALL include an example systemd service unit file.
2. THE systemd unit file SHALL include security hardening directives sufficient to achieve a reasonable score from systemd-analyze security.
3. THE systemd unit file SHALL configure WatchdogSec, and THE RPC_Proxy SHALL periodically notify systemd via a non-blocking write to the systemd notification Unix socket to confirm the event loop is alive.
4. THE systemd unit file SHALL configure Restart=on-failure so that the process is restarted on unexpected termination.
5. THE Project SHALL include an example Dockerfile that builds and runs rpcrace.
6. THE Dockerfile SHALL include a HEALTHCHECK directive that polls the HTTP statistics endpoint to detect event loop hangs.
7. THE Dockerfile SHALL use a restart-compatible configuration so that container orchestrators can restart the container on health check failure.

### Requirement 17: Dependency and Platform Requirements

**User Story:** As a developer, I want rpcrace to use lightweight, well-known libraries available in Ubuntu repositories, so that the project is easy to build and maintain.

#### Acceptance Criteria

1. THE Project SHALL be written entirely in C.
2. THE Project SHALL prefer libraries available via apt-get on Ubuntu 24.04.
3. THE Project SHALL use yyjson for JSON parsing.
4. THE Project SHALL use libzmq for ZMQ communication.
5. WHERE hash table data structures are needed, THE Project SHALL use uthash header-only library.

### Requirement 18: Unit Testing and Performance Measurement

**User Story:** As a developer, I want unit tests that verify correctness and measure performance, so that regressions and performance degradations are detected early.

#### Acceptance Criteria

1. THE Project SHALL include unit tests that verify correct operation of core components.
2. THE Project SHALL include performance benchmarks for major areas of operation including RPC forwarding latency and notification relay latency.
3. THE Build_System SHALL provide a make target to run all unit tests.

### Requirement 19: Project Documentation

**User Story:** As a system operator or developer, I want a README that explains how to build, configure, and deploy rpcrace, so that I can get the system running without reading the source code.

#### Acceptance Criteria

1. THE Project SHALL include a README.md file.
2. THE README SHALL include a brief description of what rpcrace does.
3. THE README SHALL include build instructions covering dependencies, the configure script, and make targets.
4. THE README SHALL include a documented example `rpcrace.conf` configuration file with comments explaining each option.
5. THE README SHALL include example Bitcoin node configuration for both ZMQ (zmqpubhashblock) and HTTP blocknotify integration.
6. THE README SHALL include deployment guidance covering command line usage, systemd, and Docker.

### Requirement 20: Licensing

**User Story:** As a developer or user, I want the project to be clearly licensed, so that I understand the terms under which I can use and contribute to it.

#### Acceptance Criteria

1. THE Project SHALL be licensed under the MIT License.
2. THE Project SHALL include a LICENSE file in the repository root.

### Requirement 21: Module Encapsulation

**User Story:** As a developer, I want each module to encapsulate its internal state behind functions, so that changes within a module do not ripple across the codebase.

#### Acceptance Criteria

1. A module SHALL NOT directly read or modify the internal data structures of another module.
2. Each module SHALL expose functions to query or modify its internal state where cross-module interaction is needed.
3. Inter-module communication SHALL occur through function calls defined in the module's public header file.
4. Unit tests SHALL verify module behavior exclusively through the module's public API, not by accessing internal data structures.
