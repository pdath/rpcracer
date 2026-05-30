x# Implementation Plan: rpcrace

## Overview

Incremental implementation of the rpcrace RPC proxy and block notification relay. Tasks are ordered to build foundational modules first (utilities, logging, config, event loop), then layer on the core subsystems (connections, RPC proxy, notifier, HTTP server, stats), and finally wire everything together in main.c with deployment artifacts. Each task produces compilable, testable code that integrates with prior steps.

## Tasks

- [x] 1. Foundation: utilities, logging, and configuration
  - [x] 1.1 Create project structure, Makefile, and configure script
    - Create directory layout: `src/`, `include/`, `tests/`, `deploy/`
    - Write `configure` script (POSIX shell): detect pkg-config, libzmq, required headers (`zmq.h`, `sys/epoll.h`), verify architecture (x86_64 or aarch64), write `config.mk`
    - Write `Makefile` with targets: `all`, `test`, `clean`, `install`
    - Compiler flags: `-std=c11 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -O2 -march=native -flto -D_GNU_SOURCE`
    - Linker flags: `-flto`, libs: `-lzmq -lpthread`
    - Vendor `yyjson.c`/`yyjson.h`, `uthash.h` into `include/`
    - _Requirements: 15.1, 15.2, 15.3, 15.4, 15.5, 15.6, 17.1, 17.2, 17.3, 17.4, 17.5_

  - [x] 1.2 Implement util.c/h (clock helpers, hex encoding, buffer utilities)
    - `uint64_t clock_monotonic_ns(void)` — CLOCK_MONOTONIC nanoseconds
    - `uint64_t clock_realtime_us(void)` — CLOCK_REALTIME microseconds for log timestamps
    - `int hex_encode(const uint8_t *src, size_t len, char *dst)` — bytes to hex string
    - `int hex_decode(const char *src, size_t len, uint8_t *dst)` — hex string to bytes, returns -1 on invalid input
    - _Requirements: 12.10, 2.1_

  - [x] 1.3 Implement log.c/h (non-blocking stderr logging)
    - `log_init(int verbosity)` — set stderr O_NONBLOCK via fcntl, store verbosity level
    - `log_msg(int level, const char *fmt, ...)` — format with ISO 8601 microsecond timestamp, level tag, source; single write() call; discard on EAGAIN/EWOULDBLOCK
    - `log_update_time()` — cache CLOCK_REALTIME timestamp, called once per epoll_wait dispatch iteration
    - Verbosity levels: LOG_CRIT(0), LOG_WARN(1), LOG_INFO(2), LOG_DEBUG(3)
    - Format: `<ISO8601>Z [<LEVEL>] [<source>] <message>`
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.10_

  - [x] 1.4 Write property test for log message format (Property 11)
    - **Property 11: Log Message Format**
    - Generate random messages, levels (0–3), and source identifiers; verify output contains valid ISO 8601 timestamp with microsecond precision, level tag, and source identifier
    - **Validates: Requirements 9.3, 9.4**

  - [x] 1.5 Write property test for slow response threshold (Property 12)
    - **Property 12: Slow Response Threshold Warning**
    - Generate random elapsed times around the 5-second boundary; verify warning is logged if and only if elapsed > 5s
    - **Validates: Requirements 9.9**

  - [x] 1.6 Implement config.c/h (JSON configuration parsing)
    - `config_t *config_load(const char *path)` — read file, parse with yyjson immutable API, populate config_t struct
    - `void config_destroy(config_t *cfg)` — free config
    - Struct fields: nodes[MAX_NODES] (host, rpc_port, zmq_port, label), rpc_server_bind, rpc_server_port, http_server_bind, http_server_port, zmq_server_bind, zmq_server_port, notify_http_url, rpc_timeout_ms, reconnect_delay_ms, stall_threshold_ms, log_verbosity
    - Validation: at least 1 node, non-zero ports, positive timeout, unique labels; warn if no downstream notify method configured
    - Command-line path override: if argv provides path use it, else look for `rpcrace.conf` in cwd; exit with descriptive error if file cannot be read or parsed
    - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.5, 11.6, 11.7, 11.8, 11.9, 11.10, 11.11_

  - [x] 1.7 Write property test for configuration round-trip (Property 13)
    - **Property 13: Configuration Round-Trip**
    - Generate random valid configs (1–8 nodes, random hosts/ports/labels, optional ZMQ endpoints, valid timeouts/verbosity); serialize to JSON with yyjson mutable API; parse with config_load(); verify equivalence
    - **Validates: Requirements 11.4, 11.5, 11.6, 11.7, 11.8, 11.9**

- [x] 2. Checkpoint - Verify foundation compiles and tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 3. Event loop and connection management
  - [x] 3.1 Implement event_loop.c/h (epoll wrapper and timer management)
    - `event_loop_create()` — create epoll fd, initialize timer list
    - `event_loop_add_fd/mod_fd/del_fd()` — register/modify/remove fd with epoll, store event_cb and user data
    - `event_loop_add_timer(uint64_t ms, timer_cb cb, void *data)` — create timerfd via timerfd_create(), register with epoll
    - `event_loop_run()` — epoll_wait loop, dispatch callbacks, call log_update_time() each iteration
    - `event_loop_stop()` — set running flag to false
    - `event_loop_destroy()` — close epoll fd, close all timerfds, free resources
    - Stall detection timer: fires every `stall_threshold_ms / 2`, records monotonic timestamp; if delta exceeds threshold, log critical and exit(1)
    - _Requirements: 12.1, 14.3_

  - [x] 3.2 Implement rpc_conn.c/h (upstream HTTP/1.1 connections)
    - `upstream_conn_t` state machine: DISCONNECTED → CONNECTING → CONNECTED → SENDING → RECEIVING
    - Non-blocking connect() with EPOLLOUT notification on completion
    - Socket options at connect time: TCP_NODELAY, SO_KEEPALIVE, SO_SNDBUF/SO_RCVBUF (SOCKET_BUF_SIZE = 4MB) with getsockopt() verification and warning if actual < requested
    - Send path: zero-copy pointer to shared request buffer (`send_buf`), track `send_offset`; set to NULL after race completes
    - Receive path: pre-allocated `recv_buf` (SOCKET_BUF_SIZE capacity), parse Content-Length header for response framing
    - Handle `Connection: close` header from server — tear down and schedule reconnect
    - Reconnection: exponential backoff from `reconnect_delay_ms`, doubling up to 30s cap, reset on success
    - Mid-transfer disconnect: treat as error response from that node
    - _Requirements: 12.2, 12.3, 12.4, 12.5, 12.6, 12.8, 12.9, 12.10_

  - [x] 3.3 Write property test for auth header pass-through (Property 16)
    - **Property 16: Auth Header Pass-Through**
    - Generate random Base64-encoded auth strings; construct HTTP request with Authorization header; verify upstream request contains identical header unchanged (zero-copy forwarding preserves it)
    - **Validates: Requirements 12.7**

- [x] 4. RPC proxy core logic
  - [x] 4.1 Implement rpc_proxy.c/h — client connection and request parsing
    - RPC listener: bind/listen on configured rpc_bind:rpc_port, accept single client connection
    - Client replacement: if new connection arrives while one is active, drop old, accept new (Req 13.1)
    - Request parsing: read complete HTTP request into `client_recv_buf` (pre-allocated, SOCKET_BUF_SIZE), extract JSON-RPC method name using yyjson
    - Single request in-flight enforcement: if second request arrives while race/sticky is active, log error and drop new request without processing
    - Client disconnect mid-race: let upstream requests complete, discard responses (Req 13.2)
    - _Requirements: 13.1, 13.2, 4.1_

  - [x] 4.2 Implement rpc_proxy.c — method routing and fan-out dispatch
    - Method routing table: classify method → race/sticky/broadcast strategy per design table
    - Fan-out: for race/broadcast methods, set `send_buf` pointer on all connected upstreams, register EPOLLOUT
    - Sticky: for subsequent GBT and preciousblock, send only to `sticky_node_idx`
    - preciousblock with no sticky: return RPC error immediately (Req 8.2)
    - Log unexpected method names at LOG_WARN, still process via fan-out race (Req 4.5)
    - Log RPC method name for every request at LOG_INFO (Req 9.7)
    - _Requirements: 4.1, 4.5, 5.1, 5.4, 6.1, 7.1, 8.1, 8.2, 8.3, 9.7_

  - [x] 4.3 Write property test for RPC method routing (Property 2)
    - **Property 2: RPC Method Routing Classification**
    - Generate random method name strings (including the 4 known methods and random unknowns); verify routing decision matches the specification table: getblocktemplate→race-then-sticky, submitblock→broadcast, sendrawtransaction→broadcast, preciousblock→sticky-only, others→fan-out race
    - **Validates: Requirements 4.1, 4.5, 5.1, 6.1, 7.1, 8.1**

  - [x] 4.4 Implement rpc_proxy.c — race winner selection and response handling
    - On upstream response complete: parse HTTP status and check for RPC error (parse JSON for "error" field with yyjson)
    - Race logic: first non-error response wins, send to client, mark remaining as discard-on-complete
    - All-error fallback: if all nodes error, return last error received to client
    - Broadcast (submitblock/sendrawtransaction): `all_must_complete = true`; first success returned, but all requests run to completion (never aborted)
    - After race complete: clear all upstream `send_buf` pointers to NULL
    - Log race winner at LOG_INFO (Req 9.6), log response time and metadata for GBT/submitblock (Req 9.8)
    - Log warning if elapsed > 5s identifying slow node (Req 9.9)
    - All nodes unreachable: return error immediately, log critical (Req 13.3)
    - _Requirements: 4.2, 4.3, 4.4, 6.2, 6.3, 6.4, 7.2, 7.3, 7.4, 9.6, 9.8, 9.9, 13.3_

  - [x] 4.5 Write property test for race winner selection (Property 3)
    - **Property 3: Race Winner Selection**
    - Generate random ordered sequences of responses (mix of success/error); verify first non-error in arrival order is selected as winner
    - **Validates: Requirements 4.3, 6.3, 7.3**

  - [x] 4.6 Write property test for all-error fallback (Property 4)
    - **Property 4: All-Error Fallback**
    - Generate all-error response sequences; verify last error received is returned to client
    - **Validates: Requirements 4.4, 5.6, 6.4, 7.4**

  - [x] 4.7 Implement rpc_proxy.c — GBT height validation and sticky node logic
    - Parse "height" field from GBT responses using yyjson
    - Height match: first response with height == `last_block_height + 1` wins, node becomes sticky
    - Height fallback: if no match, highest height wins (last received if tied), node becomes sticky
    - Height monotonicity: update `last_block_height` only if response height > current value
    - On block notify callback (`rpc_proxy_on_block_notify`): set `notify_pending = true`, clear `sticky_node_idx` to -1
    - First GBT before any notify (startup): race all nodes, accept first valid, set sticky (Req 5.7)
    - GBT race after all errors: return last error (Req 5.6)
    - _Requirements: 5.2, 5.3, 5.4, 5.5, 5.6, 5.7_

  - [x] 4.8 Write property test for GBT height match selection (Property 5)
    - **Property 5: GBT Height Match Selection**
    - Generate response sets with various heights including some matching `last_block_height + 1`; verify first matching response in arrival order wins and its node becomes sticky
    - **Validates: Requirements 5.2**

  - [x] 4.9 Write property test for GBT height fallback selection (Property 6)
    - **Property 6: GBT Height Fallback Selection**
    - Generate response sets where no response matches expected height; verify response with highest height wins (last received if tied)
    - **Validates: Requirements 5.3**

  - [x] 4.10 Write property test for block height monotonicity (Property 7)
    - **Property 7: Block Height Monotonicity**
    - Generate random sequences of heights; feed to height update logic; verify `last_block_height` is monotonically non-decreasing
    - **Validates: Requirements 5.5**

- [x] 5. Checkpoint - Verify RPC proxy logic compiles and property tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 6. Block notification subsystem
  - [x] 6.1 Implement notifier.c/h — ZMQ subscriptions and deduplication
    - Initialize ZMQ context and SUB sockets for each node with `zmq_port` configured
    - Subscribe to "hashblock" topic on each socket
    - Integrate ZMQ fd with epoll via `zmq_getsockopt(ZMQ_FD)`, edge-triggered; on EPOLLIN check `ZMQ_EVENTS` for `ZMQ_POLLIN`, call `zmq_recv()` in loop until drained
    - On ZMQ message: extract 32-byte hash (already in display/RPC byte order — no reversal needed), compare with `last_hash` for dedup
    - If unique: store as `last_hash`, invoke `notify_cb` callback, relay downstream
    - ZMQ reconnection handled internally by libzmq; log warning on prolonged silence
    - Tolerant of Bitcoin node restarts (Req 1.4, 1.5)
    - Log all notify messages sent/received with block hash (Req 9.5)
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 9.5_

  - [x] 6.2 Write property test for block hash deduplication (Property 1)
    - **Property 1: Block Hash Deduplication**
    - Generate random sequences of block hashes with intentional duplicates in various interleavings; verify exactly one relay per unique hash
    - **Validates: Requirements 1.3, 2.2, 3.5**

  - [x] 6.3 Implement notifier.c — downstream relay (ZMQ PUB and HTTP notify)
    - ZMQ PUB: bind socket on `zmq_server_bind:zmq_server_port`, publish multipart message matching Bitcoin Core format: topic "hashblock" (9 bytes), body (32-byte hash in display order), 4-byte LE sequence number (monotonically increasing)
    - HTTP notify: non-blocking connect/write to `notify_http_url`; substitute `%s` with 64-char hex hash if present, otherwise use URL as-is
    - Persistent HTTP connection; drop notification if write would block (no buffering/retry); log warning on failure
    - Relay only first occurrence of each unique hash (Req 3.5)
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

  - [x] 6.4 Write property test for HTTP notify URL substitution (Property 10)
    - **Property 10: HTTP Notify URL Substitution**
    - Generate random URL templates (with and without `%s`) and random valid block hashes; verify correct substitution or pass-through
    - **Validates: Requirements 3.3, 3.4**

- [x] 7. HTTP server and statistics
  - [x] 7.1 Implement http_server.c/h (minimal HTTP server for /NOTIFY and /stats)
    - Bind/listen on configured `http_bind:http_port`
    - Parse incoming HTTP GET requests: extract path
    - Route `/NOTIFY/<hex>`: validate hex (64 chars, valid hex chars), decode to 32 bytes via `hex_decode()`, invoke notifier callback; respond HTTP 200 on success
    - Invalid/missing block hash: respond HTTP 400 (Req 2.4)
    - Route `/stats`: invoke stats serialization, respond with JSON body
    - Non-blocking I/O, integrate with event loop
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 10.4_

  - [x] 7.2 Write property test for HTTP notify path parsing (Property 8)
    - **Property 8: HTTP Notify Path Parsing**
    - Generate random 32-byte values, hex-encode to 64-char strings; verify correct extraction and byte decoding from `/NOTIFY/<hex>` path
    - **Validates: Requirements 2.1**

  - [x] 7.3 Write property test for invalid block hash rejection (Property 9)
    - **Property 9: Invalid Block Hash Rejection**
    - Generate random invalid strings (wrong length, non-hex chars, empty); verify HTTP 400 rejection
    - **Validates: Requirements 2.4**

  - [x] 7.4 Implement stats.c/h (per-node metrics and JSON serialization)
    - `stats_t` structure: per-node `node_stats_t` (gbt_response_time_us, gbt_total_time_us, gbt_count, gbt_wins, gbt_last_tx_count, last_response_time_us) and global counters (last_notify_to_gbt_us, total_races, total_requests)
    - `stats_record_gbt(int node_idx, uint64_t response_time_us, uint32_t tx_count)` — update per-node metrics
    - `stats_record_race_win(int node_idx)` — increment gbt_wins
    - `stats_record_notify_to_gbt(uint64_t elapsed_us)` — record notify-to-GBT latency
    - `int stats_serialize_json(stats_t *s, config_t *cfg, char *buf, size_t cap)` — generate JSON using yyjson mutable API; include uptime, per-node metrics with labels, connected status
    - _Requirements: 10.1, 10.2, 10.3, 10.4_

  - [x] 7.5 Write property test for statistics accumulation (Property 14)
    - **Property 14: Statistics Accumulation**
    - Generate random sequences of (node_index, response_time_us, tx_count) tuples; verify gbt_count, gbt_total_time_us, and gbt_last_tx_count match expected values per node
    - **Validates: Requirements 10.1, 10.2, 10.3**

  - [x] 7.6 Write property test for statistics JSON serialization (Property 15)
    - **Property 15: Statistics JSON Serialization**
    - Generate random valid stats_t structures; serialize to JSON; deserialize with yyjson and verify field equivalence
    - **Validates: Requirements 10.4**

- [x] 8. Checkpoint - Verify notifier, HTTP server, and stats compile and tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 9. Main entry point and process lifecycle
  - [x] 9.1 Implement main.c (entry point, signal handling, event loop bootstrap)
    - Parse command-line arguments (config file path)
    - Load config via `config_load()`
    - Initialize: log, event loop, rpc_proxy, notifier, http_server, stats
    - Wire notifier callback to `rpc_proxy_on_block_notify`
    - Signal handling: SIGTERM/SIGINT → `event_loop_stop()` for clean shutdown
    - Start accepting client connections even if no nodes are reachable (Req 14.1)
    - Clean shutdown: close all connections/fds, free memory, exit 0
    - _Requirements: 14.1, 14.2, 11.2, 11.3_

  - [x] 9.2 Implement systemd watchdog notification
    - Detect `$NOTIFY_SOCKET` and `$WATCHDOG_USEC` environment variables
    - If present: add timer at WatchdogSec/2 interval, write "WATCHDOG=1" to Unix datagram socket (non-blocking sendto)
    - Write "READY=1" after initialization complete
    - _Requirements: 16.3_

- [ ] 10. Deployment artifacts and documentation
  - [-] 10.1 Create deploy/rpcrace.service (systemd unit file)
    - Type=notify for sd_notify integration
    - Security hardening: ProtectSystem=strict, ProtectHome=yes, NoNewPrivileges=yes, PrivateTmp=yes, PrivateDevices=yes, ProtectKernelTunables=yes, ProtectControlGroups=yes, RestrictSUIDSGID=yes
    - WatchdogSec configured, Restart=on-failure, RestartSec
    - _Requirements: 16.1, 16.2, 16.3, 16.4_

  - [-] 10.2 Create deploy/Dockerfile
    - Multi-stage build: build stage with gcc/libzmq3-dev/pkg-config/make, runtime stage minimal (libzmq5 only)
    - HEALTHCHECK polling /stats endpoint to detect event loop hangs
    - Restart-compatible configuration for container orchestrators
    - _Requirements: 16.5, 16.6, 16.7_

  - [-] 10.3 Create deploy/rpcrace.conf.example
    - Full example config with all options and inline comments explaining each field
    - Multiple nodes demonstrating ZMQ and HTTP-only configurations
    - _Requirements: 11.5, 11.6, 11.7, 11.8, 11.9, 11.10, 11.11_

  - [-] 10.4 Create README.md
    - Project description: what rpcrace does and why
    - Build instructions: dependencies (apt packages), configure script, make targets
    - Documented example config with explanations of each option
    - Bitcoin node configuration examples: `zmqpubhashblock` and `blocknotify=wget` integration
    - Deployment guidance: CLI usage, systemd unit, Docker
    - _Requirements: 19.1, 19.2, 19.3, 19.4, 19.5, 19.6_

  - [-] 10.5 Create LICENSE file (MIT)
    - _Requirements: 20.1, 20.2_

- [ ] 11. Performance benchmarks
  - [ ] 11.1 Create tests/bench_rpc.c (RPC forwarding latency benchmark)
    - Measure loopback RPC forwarding overhead (target < 100μs)
    - Measure notification relay latency (ZMQ receive to ZMQ publish, target < 50μs)
    - Measure JSON config parsing time for maximum-size config
    - Measure stats JSON serialization time
    - Measure log message formatting throughput (messages/second before discard)
    - _Requirements: 18.2_

- [ ] 12. Final checkpoint - Full build and test on target platform
  - Deploy to remote test machine (`odroid`) via `scp -r Makefile configure src/ include/ tests/ deploy/ odroid:~/rpcrace/`
  - Run `ssh odroid "cd ~/rpcrace && ./configure && make && make test"`
  - Ensure all tests pass on ARM64, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties from the design document (16 properties total)
- All property tests use the hand-rolled randomized pattern: seeded PRNG (srand48/lrand48), 1000 trials, seed printed for reproducibility, seed accepted via argv[1]
- Target platform is Ubuntu 24.04 / Debian 13 on ARM64 and x86_64; final testing on remote `odroid` machine via SSH
- The project is pure C11 with gcc, no external test framework
- Deploy via `scp` to remote odroid machine for integration testing (rsync not available locally)
- Local Windows machine is for design and coding only; all compilation and testing happens on odroid

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "1.3"] },
    { "id": 2, "tasks": ["1.4", "1.5", "1.6"] },
    { "id": 3, "tasks": ["1.7", "3.1"] },
    { "id": 4, "tasks": ["3.2"] },
    { "id": 5, "tasks": ["3.3", "4.1"] },
    { "id": 6, "tasks": ["4.2"] },
    { "id": 7, "tasks": ["4.3", "4.4"] },
    { "id": 8, "tasks": ["4.5", "4.6", "4.7"] },
    { "id": 9, "tasks": ["4.8", "4.9", "4.10"] },
    { "id": 10, "tasks": ["6.1"] },
    { "id": 11, "tasks": ["6.2", "6.3"] },
    { "id": 12, "tasks": ["6.4", "7.1"] },
    { "id": 13, "tasks": ["7.2", "7.3", "7.4"] },
    { "id": 14, "tasks": ["7.5", "7.6"] },
    { "id": 15, "tasks": ["9.1"] },
    { "id": 16, "tasks": ["9.2"] },
    { "id": 17, "tasks": ["10.1", "10.2", "10.3", "10.4", "10.5"] },
    { "id": 18, "tasks": ["11.1"] }
  ]
}
```
