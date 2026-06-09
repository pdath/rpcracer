# Implementation Plan: conn-pair-refactor

## Overview

Replace upstream connection management with a "connection pair" abstraction. Each upstream node gets a `conn_pair_t` managing two TCP connections (active + standby) with a fixed 10-second rotation timer. The proxy interacts through a narrow interface (`conn_pair_get_active`, `conn_pair_is_available`, `conn_pair_report_error`, `conn_pair_tick`). Remove exponential backoff, CONN_DEAD, RACE_RETRY_WAIT, and synthetic error generation.

## Tasks

- [x] 1. Strip obsolete code from rpc_conn module
  - [x] 1.1 Remove CONN_DEAD state, backoff, and reconnect scheduling from rpc_conn
    - Remove `CONN_DEAD` from `conn_state_t` enum
    - Remove `RECONNECT_DEAD_THRESHOLD` constant
    - Remove `RECONNECT_MAX_MS` constant
    - Remove `rpc_conn_schedule_reconnect()` function and its declaration
    - Remove `rpc_conn_try_reconnect()` function and its declaration
    - Remove `reconnect_attempts`, `next_reconnect_ns`, `reconnect_base_ms` fields from `upstream_conn_t`
    - Remove all calls to `rpc_conn_schedule_reconnect()` from `conn_epoll_cb` error paths
    - Remove the `CONN_DEAD` case from `conn_state_name()` and `conn_epoll_cb`
    - Modify `rpc_conn_reset()`: when `connection_close` is set, call `rpc_conn_disconnect()` only (no reconnect scheduling)
    - Modify `rpc_conn_connect()`: allow connect from `CONN_DISCONNECTED` only (remove `CONN_DEAD` check)
    - Remove `reconnect_delay_ms` parameter from `rpc_conn_init()` signature
    - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.7_

  - [x] 1.2 Remove RACE_RETRY_WAIT and synthetic errors from rpc_proxy
    - Remove `RACE_RETRY_WAIT` from `race_state_t` enum
    - Remove `enter_retry_wait()`, `retry_poll_cb()`, `retry_deadline_cb()` functions and forward declarations
    - Remove `send_rpc_error_to_client()` function and forward declaration
    - Remove `retry_timer_fd`, `retry_deadline_timer_fd`, `retry_attempts`, `is_post_notify_gbt` fields from `struct rpc_proxy`
    - Remove retry-related logic in `client_cb` dispatch (the `enter_retry_wait` call path)
    - Temporarily replace all-nodes-unreachable path with `close_client(proxy)` call
    - _Requirements: 11.5, 11.6, 11.9, 7.3, 7.4_

- [x] 2. Implement conn_pair module
  - [x] 2.1 Create conn_pair.h with the public interface
    - Define `conn_pair_t` struct with `slots[2]`, `active_idx`, `swap_required`, `rotation_timer_fd`, `loop`, `config`, `node_index`
    - Declare `conn_pair_init`, `conn_pair_destroy`, `conn_pair_get_active`, `conn_pair_is_available`, `conn_pair_report_error`, `conn_pair_tick`
    - Define `CONN_PAIR_ROTATION_INTERVAL_MS` as 10000
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 13.2_

  - [x] 2.2 Implement conn_pair_init and conn_pair_destroy
    - `conn_pair_init`: call `rpc_conn_init` for both slots, call `rpc_conn_connect` for both, arm rotation timer via `event_loop_add_fd` on a 10s timerfd
    - `conn_pair_destroy`: call `rpc_conn_destroy` on both slots, disarm and close rotation timerfd
    - Handle partial-init failure: free any allocated resources and return -1
    - _Requirements: 1.5, 2.1, 2.2, 2.3, 2.4_

  - [x] 2.3 Implement conn_pair_get_active and conn_pair_is_available
    - `conn_pair_get_active`: return `&pair->slots[pair->active_idx]` if state is `CONN_CONNECTED`, else NULL
    - `conn_pair_is_available`: return `pair->slots[pair->active_idx].state == CONN_CONNECTED`
    - _Requirements: 1.1, 1.2, 6.1_

  - [x] 2.4 Implement conn_pair_report_error
    - Swap `active_idx` unconditionally (toggle 0↔1)
    - Close old active's fd, set its state to CONN_DISCONNECTED, clear send/recv buffers
    - Return true if new active is CONN_CONNECTED, false otherwise
    - Reset rotation timer to 10s from swap event
    - _Requirements: 1.3, 4.1, 4.2, 4.3, 4.4, 4.5, 9.3_

  - [x] 2.5 Implement conn_pair_tick (rotation logic)
    - Timer callback: set `swap_required = true`
    - Check preconditions: active is CONN_CONNECTED (idle), standby is CONN_CONNECTED
    - If preconditions met: close old standby, open new connection as standby, on connect callback swap new standby to active, clear `swap_required`
    - If preconditions not met: defer (keep `swap_required = true`)
    - Also callable after request completes to check deferred rotation
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 9.1, 9.2, 9.4_

  - [x] 2.6 Implement both-connections-down recovery
    - When both slots are non-usable: `conn_pair_is_available` returns false
    - On each timer tick: attempt `rpc_conn_connect` on standby slot
    - When standby reaches CONN_CONNECTED: swap to active automatically
    - No interval increase — always 10s retry
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

  - [x] 2.7 Write property test: availability reflects active connection state
    - **Property 1: Availability reflects active connection state**
    - Generate random conn_pair_t states (all combinations of slot states and active_idx)
    - Verify `conn_pair_is_available` returns true iff `slots[active_idx].state == CONN_CONNECTED`
    - Verify `conn_pair_get_active` returns non-NULL iff same condition
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 1.1, 1.2, 6.1**

  - [x] 2.8 Write property test: error-triggered swap is unconditional
    - **Property 2: Error-triggered swap is unconditional**
    - Generate random standby states (CONN_DISCONNECTED, CONN_CONNECTING, CONN_CONNECTED)
    - Verify `conn_pair_report_error` always swaps active_idx regardless of standby state
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 1.3, 4.1, 9.3**

  - [x] 2.9 Write property test: timer-driven rotation respects preconditions
    - **Property 3: Timer-driven rotation respects preconditions**
    - Generate random (swap_required, active_state, standby_state) tuples
    - Verify rotation executes iff swap_required AND active==CONN_CONNECTED AND standby==CONN_CONNECTED
    - Verify swap_required remains true when preconditions not met
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 3.2, 3.3, 9.1, 9.2, 9.4**

- [x] 3. Checkpoint - Verify conn_pair module compiles and passes tests
  - Ensure all tests pass, ask the user if questions arise.

- [x] 4. Integrate conn_pair into rpc_proxy
  - [x] 4.1 Replace upstreams[] with pairs[] in rpc_proxy
    - Replace `upstream_conn_t upstreams[MAX_NODES]` with `conn_pair_t pairs[MAX_NODES]` in `struct rpc_proxy`
    - Replace `upstream_count` with `pair_count`
    - Update `rpc_proxy_create`: call `conn_pair_init` for each configured node
    - Update `rpc_proxy_destroy`: call `conn_pair_destroy` for each pair
    - Update `rpc_proxy_get_states`: use `conn_pair_get_active` to determine state string
    - _Requirements: 1.6, 2.1_

  - [x] 4.2 Update dispatch_fanout to use conn_pair interface
    - Iterate `pairs[]`, call `conn_pair_get_active(pair)` to get upstream_conn_t*
    - Skip pairs where `conn_pair_get_active` returns NULL
    - Call `rpc_conn_send` on the returned connection pointer
    - _Requirements: 1.6, 8.1, 8.2_

  - [x] 4.3 Update dispatch_sticky to use conn_pair interface
    - Use `conn_pair_get_active(&pairs[sticky_idx])` instead of checking `conn->state`
    - If NULL: close client connection (no synthetic error)
    - _Requirements: 1.6, 5.6, 7.5_

  - [x] 4.4 Implement single-retry error handling in on_upstream_error
    - On transport error: call `conn_pair_report_error(pair)`
    - If returns true (available): retry request once on new active connection
    - If returns false: mark node failed for this request, continue race with remaining nodes
    - On double failure: mark pair unavailable, no further retry
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.7_

  - [x] 4.5 Implement all-nodes-unavailable as close-client
    - At dispatch time: if no pair is available, call `close_client(proxy)` and reset state to RACE_IDLE
    - After race completes with all errors: call `close_client(proxy)`
    - No synthetic JSON-RPC error response
    - _Requirements: 7.1, 7.2, 7.3, 7.5_

  - [x] 4.6 Update broadcast retry logic with conn_pair swap
    - On broadcast transport failure: call `conn_pair_report_error`, retry once on new active if available
    - Double failure: mark node done for this broadcast, no further retry
    - Wait for all in-flight responses before determining result
    - _Requirements: 8.3, 8.4, 8.5, 8.6, 8.7_

  - [x] 4.7 Write property test: single retry semantics
    - **Property 5: Single retry semantics**
    - Generate request sequences with injected transport failures
    - Verify at most one retry per node per request
    - Verify double failure results in node unavailable with no further retry
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 5.2, 5.4, 5.7**

  - [x] 4.8 Write property test: broadcast targets exactly available nodes
    - **Property 8: Broadcast targets exactly available nodes**
    - Generate random availability vectors for N conn_pairs
    - Verify broadcast sends to every available pair and skips unavailable pairs
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 8.1, 8.2**

- [x] 5. Checkpoint - Verify proxy integration compiles and passes tests
  - Ensure all tests pass, ask the user if questions arise.

- [x] 6. Preserve and verify routing and race logic
  - [x] 6.1 Verify classify_method routing is unchanged
    - Confirm `classify_method` returns correct strategy for all method strings
    - Preserve ROUTE_RACE, ROUTE_STICKY, ROUTE_BROADCAST classification
    - Remove `is_post_notify_gbt` field usage from classify path
    - _Requirements: 12.1_

  - [x] 6.2 Verify GBT height winner selection is unchanged
    - Confirm `parse_gbt_height` and height comparison logic untouched
    - Confirm race winner selection picks strictly greatest height
    - _Requirements: 12.2_

  - [x] 6.3 Verify HTTP framing and JSON-RPC parsing preserved
    - Confirm `parse_http_request`, `extract_json_rpc_method` unchanged
    - Confirm socket configuration (`configure_socket`) preserved
    - _Requirements: 12.3, 12.4, 12.5, 12.6_

  - [x] 6.4 Write property test: method routing classification
    - **Property 10: Method routing classification**
    - Generate random method strings including known methods and unknown strings
    - Verify correct routing strategy for each
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 12.1**

  - [x] 6.5 Write property test: GBT height winner selection
    - **Property 11: GBT height winner selection**
    - Generate random arrays of integer heights
    - Verify winner is the response with strictly greatest height value
    - Minimum 100 iterations using theft library
    - **Validates: Requirements 12.2**

- [x] 7. Update Makefile and existing tests
  - [x] 7.1 Add conn_pair.c to Makefile build
    - Add `conn_pair.c` / `conn_pair.o` to SRCS/OBJS
    - Ensure test binaries link against conn_pair.o
    - _Requirements: 11.8_

  - [x] 7.2 Update existing tests that reference removed identifiers
    - Find tests referencing CONN_DEAD, rpc_conn_schedule_reconnect, rpc_conn_try_reconnect, RACE_RETRY_WAIT, send_rpc_error_to_client
    - Present failing tests to operator for review before modifying
    - _Requirements: 10.1, 10.2, 10.3_

  - [x] 7.3 Write unit tests for conn_pair rotation and swap
    - Test rotation timer fires and sets swap_required
    - Test successful rotation sequence
    - Test failed standby retains current active
    - Test error swap closes old primary fd and resets state
    - _Requirements: 3.1, 3.2, 3.7, 4.4_

- [x] 8. Final checkpoint - Full build and test pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests use the theft library (C property-based testing) with minimum 100 iterations per property
- Unit tests validate specific examples and edge cases
- Requirement 10 (test failure review) is enforced in task 7.2 — failing tests are presented before modification
- The remote ARM64 machine (`ssh odroid`) is the build/test target; local Windows is edit-only
- All source files must use LF line endings

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "1.2"] },
    { "id": 1, "tasks": ["2.1"] },
    { "id": 2, "tasks": ["2.2", "2.3", "2.4", "2.5", "2.6"] },
    { "id": 3, "tasks": ["2.7", "2.8", "2.9", "7.1"] },
    { "id": 4, "tasks": ["4.1"] },
    { "id": 5, "tasks": ["4.2", "4.3", "4.4", "4.5", "4.6"] },
    { "id": 6, "tasks": ["4.7", "4.8", "6.1", "6.2", "6.3"] },
    { "id": 7, "tasks": ["6.4", "6.5", "7.2", "7.3"] }
  ]
}
```
