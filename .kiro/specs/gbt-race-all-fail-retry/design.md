# GBT Race All-Fail Retry Bugfix Design

## Overview

When a block notify arrives, the proxy clears sticky state and the next `getblocktemplate` request triggers a fan-out race to all upstream nodes. If every node fails simultaneously (connection errors, IBD, or connections being recycled), the proxy immediately returns an RPC error and transitions to `RACE_IDLE`. The downstream stratum proxy (ckpool) then waits its own 60-second timeout before retrying, causing unacceptable GBT latency.

The fix adds a bounded retry loop specifically for post-notify GBT races. When all nodes fail, instead of immediately returning an error, the proxy enters a `RACE_RETRY_WAIT` state that uses a one-shot timerfd to periodically check if any node has reconnected. If a node reconnects within the retry window, the GBT request is re-dispatched. If the retry timeout expires, the original error behavior is preserved.

## Glossary

- **Bug_Condition (C)**: A post-notify GBT fan-out race where ALL upstream nodes fail simultaneously (connection errors, IBD responses, or `dispatch_fanout` returns 0)
- **Property (P)**: The proxy SHALL retry the GBT request within a bounded time window rather than immediately returning an error, leveraging the existing reconnect timer to recover
- **Preservation**: All non-GBT race methods, successful GBT races, sticky GBT requests, broadcast methods, and the RPC timeout mechanism must remain unchanged
- **`on_upstream_error`**: Callback in `rpc_proxy.c` invoked when an upstream connection fails mid-transfer during a race
- **`dispatch_fanout`**: Function in `rpc_proxy.c` that sends the client request to all `CONN_CONNECTED` upstream nodes; returns the count dispatched
- **`race_complete`**: Function in `rpc_proxy.c` that resets all upstream connections and transitions to `RACE_IDLE`
- **`notify_pending`**: Boolean flag set by `rpc_proxy_on_block_notify`; cleared when the next GBT is classified as `ROUTE_RACE`
- **`RACE_RETRY_WAIT`**: New race state indicating the proxy is waiting for a node to reconnect before re-dispatching a failed post-notify GBT request
- **`gbt_retry_timeout_ms`**: New configurable parameter (default 5000ms) controlling the maximum retry window duration

## Bug Details

### Bug Condition

The bug manifests when a block notify arrives, the subsequent GBT race is dispatched to all upstream nodes, and every node either fails with a connection error, returns an IBD error (-10), or is not in `CONN_CONNECTED` state at dispatch time. The `on_upstream_error` callback decrements `responses_pending` to 0 with no winner, triggering `send_rpc_error_to_client` and `race_complete` immediately — with no opportunity for the reconnect timer (which fires every 1 second) to recover any connection.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type RpcRequest
  OUTPUT: boolean

  LET method = input.method
  LET trigger = input.post_notify  // true if notify_pending was set when classified
  LET dispatched = countConnectedNodes(input.upstreams)
  LET all_failed = (dispatched == 0) OR
                   (allResponsesAreErrors(input.upstreams) AND responsesComplete(input))

  RETURN method == "getblocktemplate"
         AND trigger == true
         AND all_failed
         AND retryNotAttempted(input)
END FUNCTION
```

### Examples

- **All nodes disconnected at dispatch time**: Block notify arrives, client sends GBT, `dispatch_fanout` finds 0 nodes in `CONN_CONNECTED` → immediate "All upstream nodes unreachable" error. A node reconnects 800ms later but the error was already sent.
- **All nodes fail mid-transfer**: Block notify arrives, 3 nodes dispatched, all 3 return connection errors within 200ms (remote nodes in IBD, local node recycling its RPC socket) → `on_upstream_error` fires 3 times, `responses_pending` hits 0, error sent. Nodes reconnect 1.2s later.
- **Mixed failure: IBD + connection error**: Node A returns error -10 (IBD), Node B connection drops → both counted as errors, `responses_pending` hits 0, error sent immediately.
- **Edge case — single node configured**: Only one upstream node, it disconnects during the race → same behavior, error sent immediately instead of waiting for reconnect.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Non-GBT methods (`validateaddress`, `decoderawtransaction`) that fail must continue returning errors immediately
- Broadcast methods (`submitblock`, `sendrawtransaction`) that fail must continue returning errors immediately
- GBT races that succeed on the first attempt (at least one node responds successfully) must continue selecting a winner by height matching
- Sticky GBT requests that fail must continue falling back to fan-out race (no retry on sticky path)
- The RPC timeout mechanism for normal races with pending responses must continue firing and calling `race_complete`
- Block notify arriving during a retry-in-progress must continue to honor sticky clearing (the retry completes for the current block)

**Scope:**
All inputs that do NOT involve a post-notify GBT race with all nodes failing should be completely unaffected by this fix. This includes:
- Any non-`getblocktemplate` method
- GBT requests that are classified as `ROUTE_STICKY` (not post-notify)
- GBT races where at least one node responds successfully
- All broadcast/all-must-complete races

## Hypothesized Root Cause

Based on the code analysis, the root cause is clear and confirmed:

1. **No retry path in `on_upstream_error`**: When `responses_pending` reaches 0 with `winner_idx == -1`, the code unconditionally calls `send_rpc_error_to_client` and `race_complete`. There is no check for whether this is a post-notify GBT race that should be retried.

2. **No retry path in `dispatch_fanout` return-0 handling**: In `client_cb`, when `dispatch_fanout` returns 0 (no nodes connected), the code immediately sends "All upstream nodes unreachable" and sets `state = RACE_IDLE`. There is no waiting mechanism.

3. **Timing mismatch between reconnect and error delivery**: The reconnect timer fires every 1 second and can recover connections within 1-2 seconds. But the error is sent to the client within milliseconds of the last node failing, before any reconnection can occur.

4. **No state to track "should retry"**: The race state machine has only `RACE_IDLE`, `RACE_FANOUT`, and `RACE_STICKY`. There is no state representing "waiting to retry after all-fail".

## Correctness Properties

Property 1: Bug Condition - Post-Notify GBT All-Fail Retries

_For any_ post-notify GBT race where all upstream nodes fail simultaneously (isBugCondition returns true), the fixed proxy SHALL enter a bounded retry loop (state `RACE_RETRY_WAIT`) and wait up to `gbt_retry_timeout_ms` for at least one node to reconnect, re-dispatching the GBT request upon reconnection rather than immediately returning an RPC error.

**Validates: Requirements 2.1, 2.2, 2.3**

Property 2: Preservation - Non-Retry Path Behavior

_For any_ RPC request where the bug condition does NOT hold (non-GBT methods, successful GBT races, sticky GBT, broadcast methods, or non-post-notify GBT races), the fixed code SHALL produce exactly the same behavior as the original code, preserving immediate error delivery for non-GBT all-fail cases and immediate winner selection for successful races.

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6**

## Fix Implementation

### Changes Required

Assuming our root cause analysis is correct:

**File**: `src/rpc_proxy.c`

**Function**: `on_upstream_error`, `client_cb`, and new helper functions

**Specific Changes**:

1. **Add `RACE_RETRY_WAIT` state**: Extend the `race_state_t` enum with a new state representing "waiting to retry a failed post-notify GBT race".

2. **Add retry tracking fields to `rpc_proxy` struct**:
   - `int retry_timer_fd` — one-shot timerfd for the retry check interval (100ms)
   - `int retry_deadline_timer_fd` — one-shot timerfd for the overall retry timeout
   - `int retry_attempts` — number of re-dispatch attempts made
   - `bool is_post_notify_gbt` — flag set when classifying a GBT race triggered by `notify_pending`

3. **Modify `on_upstream_error` all-fail path**: When `responses_pending` reaches 0, `winner_idx == -1`, method is `getblocktemplate`, and `is_post_notify_gbt` is true — instead of calling `send_rpc_error_to_client` + `race_complete`, call a new `enter_retry_wait(proxy)` function.

4. **Modify `client_cb` dispatch_fanout-returns-0 path**: For post-notify GBT races (method is `getblocktemplate` and `is_post_notify_gbt` is true), instead of immediately sending an error, call `enter_retry_wait(proxy)`.

5. **New function `enter_retry_wait(proxy)`**: 
   - Transition to `RACE_RETRY_WAIT` state
   - Reset all upstream connections (call `rpc_conn_reset` on participants)
   - Cancel the existing RPC timeout timer
   - Create a recurring 100ms timerfd (`retry_timer_fd`) to poll for reconnected nodes
   - Create a one-shot `gbt_retry_timeout_ms` timerfd (`retry_deadline_timer_fd`) as the absolute deadline
   - Log at `LOG_INFO` that retry wait has begun

6. **New callback `retry_poll_cb`**: Fired every 100ms by `retry_timer_fd`:
   - Scan `proxy->upstreams[]` for any node in `CONN_CONNECTED` state
   - If found: cancel both retry timers, call `dispatch_fanout(proxy)`, if dispatched > 0 set state to `RACE_FANOUT` and `start_rpc_timeout(proxy)`, increment `retry_attempts`
   - If `dispatch_fanout` still returns 0: remain in `RACE_RETRY_WAIT` (wait for next poll)

7. **New callback `retry_deadline_cb`**: Fired once when `gbt_retry_timeout_ms` expires:
   - Cancel the retry poll timer
   - Send RPC error to client ("All upstream nodes unreachable after retry timeout")
   - Call `race_complete(proxy)`
   - Log at `LOG_CRIT`

8. **Handle block notify during retry**: `rpc_proxy_on_block_notify` should NOT abort the retry — the retry is for the current block's GBT. The sticky clearing still happens, and when the retry succeeds, the response will be for the current block.

**File**: `src/config.c` / `src/config.h`

9. **Add `gbt_retry_timeout_ms` config field**: 
   - Add `uint32_t gbt_retry_timeout_ms` to `config_t` struct
   - Parse from JSON config (optional, default 5000ms)
   - Validate range: 0 (disabled) to 30000ms

## Testing Strategy

### Validation Approach

The testing strategy follows a two-phase approach: first, surface counterexamples that demonstrate the bug on unfixed code, then verify the fix works correctly and preserves existing behavior.

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bug BEFORE implementing the fix. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: Write a test that sets up a proxy with multiple upstream nodes, simulates a block notify, then triggers a GBT request while forcing all nodes into `CONN_DISCONNECTED` state. Observe that the proxy immediately returns an error without any retry attempt. Run on UNFIXED code to confirm the behavior.

**Test Cases**:
1. **All nodes disconnected at dispatch**: Force all upstreams to `CONN_DISCONNECTED`, send GBT after notify → observe immediate error (will fail on unfixed code by showing no retry)
2. **All nodes fail mid-transfer**: Connect all nodes, dispatch GBT, then fire `on_upstream_error` for each → observe immediate error when `responses_pending` hits 0 (will fail on unfixed code)
3. **Single node configured, disconnects**: Only one upstream, force disconnect → observe immediate error (will fail on unfixed code)
4. **dispatch_fanout returns 0 after notify**: All nodes in non-CONNECTED state at dispatch time → observe immediate "unreachable" error (will fail on unfixed code)

**Expected Counterexamples**:
- The proxy sends RPC error within milliseconds of all-fail, with no delay or retry
- `race_complete` is called immediately, transitioning to `RACE_IDLE` before any node can reconnect
- Possible causes: direct path from `responses_pending == 0` to `send_rpc_error_to_client` with no conditional check

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed function produces the expected behavior.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := handleGbtRace_fixed(input)
  ASSERT state_transitions_to(RACE_RETRY_WAIT)
  ASSERT retry_timer_created()
  ASSERT retry_deadline_timer_created()
  IF node_reconnects_within_timeout(input) THEN
    ASSERT gbt_redispatched_to_reconnected_node()
    ASSERT client_receives_gbt_response()
    ASSERT state_transitions_to(RACE_IDLE)
  ELSE
    ASSERT client_receives_error_after_timeout()
    ASSERT state_transitions_to(RACE_IDLE)
  END IF
END FOR
```

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed function produces the same result as the original function.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT handleRpcRequest_original(input) = handleRpcRequest_fixed(input)
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many combinations of method types, node states, and race outcomes
- It catches edge cases where the retry logic might accidentally trigger for non-GBT methods
- It provides strong guarantees that broadcast, sticky, and non-notify GBT paths are unchanged

**Test Plan**: Observe behavior on UNFIXED code first for non-GBT methods and successful GBT races, then write property-based tests capturing that behavior.

**Test Cases**:
1. **Non-GBT method all-fail preservation**: Generate random non-GBT methods (`validateaddress`, `decoderawtransaction`, `submitblock`, `sendrawtransaction`), force all nodes to fail, verify error is returned immediately (no retry wait state)
2. **Successful GBT race preservation**: Generate random scenarios where at least one node responds successfully to a GBT race, verify winner selection and client response are unchanged
3. **Sticky GBT preservation**: Generate random scenarios where a sticky GBT is dispatched, verify fallback-to-race behavior is unchanged when sticky node is unreachable
4. **RPC timeout preservation**: Verify that the RPC timeout mechanism continues to fire and call `race_complete` for normal (non-retry) races with pending responses

### Unit Tests

- Test `enter_retry_wait` transitions proxy to `RACE_RETRY_WAIT` and creates both timers
- Test `retry_poll_cb` re-dispatches when a node is `CONN_CONNECTED`
- Test `retry_poll_cb` does nothing when no node is `CONN_CONNECTED`
- Test `retry_deadline_cb` sends error and calls `race_complete`
- Test that `is_post_notify_gbt` is only true for GBT classified via `notify_pending`
- Test config parsing of `gbt_retry_timeout_ms` with default, explicit, and 0 (disabled) values

### Property-Based Tests

- Generate random sequences of (method, node_states, race_outcomes) and verify that retry logic ONLY activates for post-notify GBT all-fail scenarios
- Generate random retry timings (node reconnects at various offsets within the window) and verify the re-dispatch occurs correctly
- Generate random configurations (1-16 nodes, various timeout values) and verify the retry deadline is respected

### Integration Tests

- Full flow: block notify → GBT request → all nodes fail → node reconnects within 2s → GBT response delivered to client
- Full flow: block notify → GBT request → all nodes fail → no reconnect within timeout → error delivered to client
- Full flow: block notify → GBT request → 1 node succeeds → normal winner selection (no retry triggered)
- Rapid block notifies: first notify triggers retry, second notify arrives during retry → verify sticky is cleared and retry still completes for current block
