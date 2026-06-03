# Implementation Plan

## Overview

Fix the GBT race all-fail immediate error behavior by adding a bounded retry loop (`RACE_RETRY_WAIT` state) that waits for upstream nodes to reconnect before returning an error to the client. The workflow follows the bug condition methodology: explore the bug with a failing test, preserve existing behavior, implement the fix, then validate.

## Tasks

- [x] 1. Write bug condition exploration test
  - **Property 1: Bug Condition** - Post-Notify GBT All-Fail Immediate Error
  - **CRITICAL**: This test MUST FAIL on unfixed code - failure confirms the bug exists
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior - it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the proxy immediately returns an error instead of retrying
  - **Scoped PBT Approach**: Scope the property to post-notify GBT races where all nodes are disconnected or fail simultaneously
  - Bug Condition from design: `isBugCondition(input)` where `method == "getblocktemplate" AND post_notify == true AND all_failed == true`
  - Test setup: Create proxy with 1-8 nodes (randomized), simulate block notify (`rpc_proxy_on_block_notify`), force all upstream nodes to `CONN_DISCONNECTED` state, send GBT request
  - Property assertion: After all nodes fail, the proxy MUST be in `RACE_RETRY_WAIT` state (not `RACE_IDLE`) and MUST NOT have sent an error response to the client yet
  - Secondary assertion: A retry timer and deadline timer must be active
  - Use hand-rolled randomized testing (srand48/lrand48, 1000 trials) consistent with project test conventions
  - Randomize: node count (1-16), which nodes are in IBD vs disconnected vs connection-error, timing of failures
  - Run test on UNFIXED code
  - **EXPECTED OUTCOME**: Test FAILS — the proxy transitions to `RACE_IDLE` and sends error immediately (no `RACE_RETRY_WAIT` state exists yet)
  - Document counterexamples: e.g., "3 nodes configured, all CONN_DISCONNECTED, GBT after notify → immediate error, state=RACE_IDLE, no retry attempted"
  - Mark task complete when test is written, run, and failure is documented
  - _Requirements: 1.1, 1.2, 1.3_

- [x] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** - Non-Retry Path Behavior Unchanged
  - **IMPORTANT**: Follow observation-first methodology
  - **Observe on UNFIXED code**:
    - Non-GBT methods (`validateaddress`, `decoderawtransaction`) with all-fail → immediate error returned, state = `RACE_IDLE`
    - Broadcast methods (`submitblock`, `sendrawtransaction`) with all-fail → immediate error returned, state = `RACE_IDLE`
    - GBT race where at least one node succeeds → winner selected by height, response sent to client, sticky set
    - Sticky GBT with sticky node unreachable → falls back to fan-out race (no retry wait)
    - RPC timeout fires during normal race with pending responses → timeout error sent, `race_complete` called
  - Write property-based tests (1000 trials each, seeded PRNG):
    - Sub-property A: For all non-GBT methods (randomly generated from known set), all-fail returns error immediately with no retry state
    - Sub-property B: For all broadcast methods with all-fail, error returned immediately with no retry state
    - Sub-property C: For GBT races where at least 1 node responds successfully (randomize which node, response timing), winner selection by height is unchanged
    - Sub-property D: For sticky GBT requests where sticky node is unreachable, fallback to fan-out race occurs (no retry wait inserted)
    - Sub-property E: RPC timeout fires during a race with pending responses → error sent and `race_complete` called (timeout not intercepted by retry logic)
  - Verify all tests PASS on UNFIXED code (these capture existing correct behavior)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

- [x] 3. Implement the GBT race all-fail retry fix

  - [x] 3.1 Add `RACE_RETRY_WAIT` state and retry fields to proxy struct
    - Add `RACE_RETRY_WAIT` to `race_state_t` enum in `rpc_proxy.c`
    - Add fields to `struct rpc_proxy`: `int retry_timer_fd`, `int retry_deadline_timer_fd`, `int retry_attempts`, `bool is_post_notify_gbt`
    - Initialize new fields in `rpc_proxy_create` (`retry_timer_fd = -1`, `retry_deadline_timer_fd = -1`, `retry_attempts = 0`, `is_post_notify_gbt = false`)
    - Clean up timerfds in `rpc_proxy_destroy`
    - _Bug_Condition: isBugCondition(input) where method=="getblocktemplate" AND post_notify==true AND all_failed==true_
    - _Requirements: 2.1_

  - [x] 3.2 Add `gbt_retry_timeout_ms` config field
    - Add `uint32_t gbt_retry_timeout_ms` to `config_t` struct in `config.h`
    - Parse from JSON in `config.c` (optional field, default 5000ms)
    - Validate range: 0 (retry disabled) to 30000ms
    - Add to `deploy/rpcrace.conf.example` and `deploy/rpcrace.conf.README`
    - _Requirements: 2.1, 2.3, 2.4_

  - [x] 3.3 Set `is_post_notify_gbt` flag in `classify_method`
    - In `classify_method`, when method is `getblocktemplate` and `notify_pending` is true (about to return `ROUTE_RACE`), set `proxy->is_post_notify_gbt = true`
    - For all other paths (non-GBT, sticky GBT, broadcast), set `proxy->is_post_notify_gbt = false`
    - _Requirements: 2.1, 3.1, 3.4_

  - [x] 3.4 Implement `enter_retry_wait(proxy)` function
    - Transition state to `RACE_RETRY_WAIT`
    - Reset all upstream connections (`rpc_conn_reset` on each participant)
    - Cancel existing RPC timeout timer via `cancel_rpc_timeout(proxy)`
    - Create one-shot 100ms timerfd for `retry_timer_fd`, register with event loop using `retry_poll_cb`
    - Create one-shot `cfg->gbt_retry_timeout_ms` timerfd for `retry_deadline_timer_fd`, register with `retry_deadline_cb`
    - Log at `LOG_INFO`: "Entering retry wait for post-notify GBT (timeout=%u ms)"
    - If `gbt_retry_timeout_ms == 0` (disabled): skip retry, fall through to original error behavior
    - _Bug_Condition: isBugCondition(input) triggers this function_
    - _Expected_Behavior: state transitions to RACE_RETRY_WAIT, timers armed_
    - _Requirements: 2.1, 2.3_

  - [x] 3.5 Implement `retry_poll_cb` callback
    - Fired every 100ms by `retry_timer_fd` (re-arm after each fire for recurring behavior)
    - Scan `proxy->upstreams[]` for any node in `CONN_CONNECTED` state
    - If found: cancel both retry timers (close fds, remove from event loop), call `dispatch_fanout(proxy)`
      - If `dispatch_fanout` returns > 0: set state to `RACE_FANOUT`, call `start_rpc_timeout(proxy)`, increment `retry_attempts`, log at `LOG_INFO`
      - If `dispatch_fanout` returns 0 (node disconnected between check and send): re-arm retry timer, remain in `RACE_RETRY_WAIT`
    - If no node connected: re-arm the 100ms timer (remain in `RACE_RETRY_WAIT`)
    - _Expected_Behavior: re-dispatches GBT when node reconnects_
    - _Preservation: Only activates when state == RACE_RETRY_WAIT_
    - _Requirements: 2.2, 2.5_

  - [x] 3.6 Implement `retry_deadline_cb` callback
    - Fired once when `gbt_retry_timeout_ms` expires
    - Cancel the retry poll timer (close fd, remove from event loop)
    - Send RPC error to client: `send_rpc_error_to_client(proxy, -1, "All upstream nodes unreachable after retry timeout")`
    - Call `race_complete(proxy)` to transition to `RACE_IDLE`
    - Log at `LOG_CRIT`: "GBT retry timeout expired after %u ms, %d attempts"
    - _Expected_Behavior: error sent only after timeout exhausted_
    - _Requirements: 2.4_

  - [x] 3.7 Modify `on_upstream_error` all-fail path
    - In `on_upstream_error`: when `responses_pending` reaches 0, `winner_idx == -1`, method is `getblocktemplate`, and `is_post_notify_gbt == true`:
      - Call `enter_retry_wait(proxy)` instead of `send_rpc_error_to_client` + `race_complete`
    - All other conditions (non-GBT, non-post-notify, broadcast) retain original immediate-error behavior
    - _Bug_Condition: isBugCondition triggers enter_retry_wait instead of immediate error_
    - _Preservation: non-GBT and non-post-notify paths unchanged_
    - _Requirements: 2.1, 3.1, 3.4_

  - [x] 3.8 Modify `client_cb` dispatch_fanout-returns-0 path
    - In `client_cb` ROUTE_RACE branch: when `dispatch_fanout` returns 0, method is `getblocktemplate`, and `is_post_notify_gbt == true`:
      - Call `enter_retry_wait(proxy)` instead of immediate error + `RACE_IDLE`
    - All other methods (non-GBT, broadcast) retain original immediate-error behavior
    - _Bug_Condition: isBugCondition triggers enter_retry_wait when no nodes available at dispatch_
    - _Preservation: non-GBT dispatch-0 path unchanged_
    - _Requirements: 2.3, 3.1, 3.4_

  - [x] 3.9 Handle block notify during retry
    - In `rpc_proxy_on_block_notify`: do NOT cancel the retry timers or abort the retry
    - The retry is for the current block's GBT — sticky clearing still happens via existing logic
    - When the retry succeeds and the GBT response comes back, it will be for the current (latest) block
    - Verify: `notify_pending` is cleared normally when the retried GBT is classified, sticky state cleared as before
    - _Preservation: Block notify during retry does not abort the retry_
    - _Requirements: 3.6_

  - [x] 3.10 Verify bug condition exploration test now passes
    - **Property 1: Expected Behavior** - Post-Notify GBT All-Fail Enters Retry
    - **IMPORTANT**: Re-run the SAME test from task 1 - do NOT write a new test
    - The test from task 1 asserts `RACE_RETRY_WAIT` state and active timers
    - With the fix implemented, the proxy should now enter retry wait instead of immediate error
    - Run bug condition exploration test from step 1
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed - proxy retries instead of failing immediately)
    - _Requirements: 2.1, 2.2, 2.3_

  - [x] 3.11 Verify preservation tests still pass
    - **Property 2: Preservation** - Non-Retry Path Behavior Unchanged
    - **IMPORTANT**: Re-run the SAME tests from task 2 - do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms non-GBT methods, successful GBT races, broadcast methods, sticky GBT, and RPC timeout behavior are all unchanged)
    - Confirm all tests still pass after fix (no regressions)

- [x] 4. Write unit tests for new functions
  - Test `enter_retry_wait` transitions proxy to `RACE_RETRY_WAIT` and creates both timers (retry_timer_fd != -1, retry_deadline_timer_fd != -1)
  - Test `retry_poll_cb` re-dispatches when a node reaches `CONN_CONNECTED` state
  - Test `retry_poll_cb` does nothing (remains in `RACE_RETRY_WAIT`) when no node is `CONN_CONNECTED`
  - Test `retry_deadline_cb` sends error and calls `race_complete` (state transitions to `RACE_IDLE`)
  - Test that `is_post_notify_gbt` is true only for GBT classified when `notify_pending` was set
  - Test that `is_post_notify_gbt` is false for non-GBT methods, sticky GBT, and broadcast methods
  - Test config parsing: `gbt_retry_timeout_ms` absent → default 5000, explicit value → parsed correctly, value 0 → retry disabled, value > 30000 → rejected
  - Test retry disabled (`gbt_retry_timeout_ms = 0`): all-fail post-notify GBT returns error immediately (no retry)
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

- [-] 5. Write integration tests
  - Full flow: block notify → GBT request → all nodes disconnected → node reconnects within 2s → GBT response delivered to client (verify response content is valid GBT)
  - Full flow: block notify → GBT request → all nodes fail mid-transfer (on_upstream_error path) → node reconnects within timeout → re-dispatch succeeds
  - Full flow: block notify → GBT request → all nodes fail → no reconnect within timeout → error delivered to client after `gbt_retry_timeout_ms`
  - Full flow: block notify → GBT request → 1 of 3 nodes succeeds immediately → normal winner selection, no retry triggered (verify `RACE_RETRY_WAIT` never entered)
  - Rapid block notifies: first notify triggers retry-in-progress → second notify arrives → verify sticky cleared, retry still completes for current block
  - Single node configuration: node disconnects during post-notify GBT → enters retry → node reconnects → GBT dispatched and response delivered
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 3.2, 3.6_

- [~] 6. Checkpoint - Ensure all tests pass
  - Deploy to remote test machine: `scp -r Makefile configure src/ include/ tests/ deploy/ odroid:~/rpcrace/`
  - Build on remote: `ssh odroid "cd ~/rpcrace && ./configure && make clean && make"`
  - Run full test suite: `ssh odroid "cd ~/rpcrace && make test"`
  - Run with sanitizers: `ssh odroid "cd ~/rpcrace && make sanitise"`
  - Ensure all tests pass (exploration test, preservation tests, unit tests, integration tests, and all existing tests)
  - Ask the user if questions arise

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": ["1", "2"],
      "description": "Exploration and preservation tests (run BEFORE fix)"
    },
    {
      "wave": 2,
      "tasks": ["3.1", "3.2"],
      "description": "Add new state enum, retry fields, and config field"
    },
    {
      "wave": 3,
      "tasks": ["3.3", "3.4"],
      "description": "Set post-notify flag and implement enter_retry_wait"
    },
    {
      "wave": 4,
      "tasks": ["3.5", "3.6", "3.7", "3.8", "3.9"],
      "description": "Implement callbacks and modify error paths"
    },
    {
      "wave": 5,
      "tasks": ["3.10", "3.11"],
      "description": "Verify exploration and preservation tests pass after fix"
    },
    {
      "wave": 6,
      "tasks": ["4"],
      "description": "Unit tests for new functions"
    },
    {
      "wave": 7,
      "tasks": ["5"],
      "description": "Integration tests for full retry flows"
    },
    {
      "wave": 8,
      "tasks": ["6"],
      "description": "Final checkpoint - all tests pass"
    }
  ]
}
```

## Notes

- The project uses hand-rolled randomized property testing (srand48/lrand48, 1000 trials per property, seed printed for reproducibility, seed accepted via argv[1])
- All tests compile as standalone C programs linked against non-main project objects
- The remote test machine (odroid, ARM64 Ubuntu) is the target platform for building and running tests
- Files must use LF line endings (no CRLF)
- The retry poll interval (100ms) is a hardcoded constant, not configurable — only the deadline timeout is user-configurable
