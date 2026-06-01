# Race State Blocking Fix — Bugfix Design

## Overview

After a non-broadcast race (e.g., `validateaddress`, non-notify `getblocktemplate`) finds a winner and sends the response to the client, the proxy remains in `RACE_FANOUT` state while waiting for remaining upstream nodes to respond. Any new client request arriving during this window is dropped with a warning log, causing the downstream client (ckpool) to time out. The fix introduces an early transition to `RACE_IDLE` immediately after the winning response is sent, while allowing late upstream responses to arrive, be logged, and be discarded without blocking new request processing.

## Glossary

- **Bug_Condition (C)**: A non-broadcast race has found a winner (response sent to client) AND at least one upstream node has not yet responded — the proxy remains in `RACE_FANOUT` and rejects new requests
- **Property (P)**: After sending the winning response, the proxy SHALL immediately accept new client requests without waiting for remaining upstream responses
- **Preservation**: Broadcast races (`submitblock`, `sendrawtransaction`) must continue to wait for all nodes; sticky requests must continue to block; late response logging (timing, height) must continue to work
- **`rpc_proxy_t`**: The main proxy structure in `src/rpc_proxy.c` holding race state, upstream connections, and client connection
- **`race_state_t`**: Enum (`RACE_IDLE`, `RACE_FANOUT`, `RACE_STICKY`) controlling whether new requests are accepted
- **`on_upstream_response()`**: Callback in `src/rpc_proxy.c` invoked when an upstream node delivers a complete HTTP response
- **`race_complete()`**: Function that resets all upstream connections and transitions to `RACE_IDLE`
- **`all_must_complete`**: Boolean flag set for broadcast methods; when true, the proxy waits for all nodes before completing

## Bug Details

### Bug Condition

The bug manifests when a non-broadcast fan-out race finds a winner (first non-error response) but other upstream nodes have not yet responded. The proxy stays in `RACE_FANOUT` state, and any new client request arriving before all pending nodes respond is silently dropped.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type {proxy_state, new_request_arrives, winner_sent, all_must_complete, responses_pending}
  OUTPUT: boolean

  RETURN proxy_state == RACE_FANOUT
         AND winner_sent == true
         AND all_must_complete == false
         AND responses_pending > 0
         AND new_request_arrives == true
END FUNCTION
```

### Examples

- `validateaddress` dispatched to 3 nodes; node[0] responds successfully in 5ms, response sent to client; node[1] and node[2] still pending (200ms away). Client sends `getblocktemplate` 10ms later → **dropped** (state=1). Expected: accepted and dispatched normally.
- `getblocktemplate` race with height match on node[1] at 15ms; response sent to client; node[0] and node[2] still pending. Client sends `validateaddress` 20ms later → **dropped**. Expected: accepted and dispatched.
- `decoderawtransaction` dispatched to 2 nodes; node[0] responds in 3ms; node[1] has 2000ms timeout remaining. Client sends next request 50ms later → **dropped** for up to 1950ms. Expected: accepted immediately.
- `submitblock` dispatched to 3 nodes; node[0] responds in 10ms; node[1] and node[2] still pending. Client sends next request → **correctly blocked** (broadcast, `all_must_complete=true`). This is NOT a bug.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Broadcast races (`submitblock`, `sendrawtransaction`) must continue to wait for all nodes to respond before accepting new requests
- Sticky requests (`preciousblock`, subsequent `getblocktemplate`) must continue to block until the sticky response arrives
- Late upstream responses must still be received, logged with timing info (`elapsed_us`, `since_notify_us` for GBT), and discarded
- The `race_complete()` function must still be called once all pending responses arrive (or timeout fires), resetting all upstream connections
- The RPC timeout timer must continue to fire and handle truly unresponsive nodes
- GBT height-match logging for all responding nodes must continue to work
- Stats recording (`stats_record_race_win`, `stats_record_gbt`, etc.) must continue for all responses

**Scope:**
All inputs that do NOT involve a non-broadcast race with a winner already sent should be completely unaffected by this fix. This includes:
- Broadcast races (all_must_complete=true)
- Sticky requests
- Races where no winner has been found yet (all errors so far)
- Races where all nodes have already responded
- The timeout path

## Hypothesized Root Cause

Based on the bug description and code analysis, the root cause is in `on_upstream_response()` at the end of the function (around line 1090 in `rpc_proxy.c`):

1. **Missing early state transition**: When a non-broadcast race finds a winner (`proxy->winner_idx != -1`) and `responses_pending > 0`, the code logs "Winner found, %d responses still pending (will discard)" but does NOT transition `proxy->state` back to `RACE_IDLE`. The state remains `RACE_FANOUT`.

2. **Overly strict request gate**: In `client_cb()`, the check `if (proxy->state != RACE_IDLE)` unconditionally drops any new request when the state is not idle. There is no distinction between "waiting for a winner" and "winner already sent, just draining late responses."

3. **Coupled concerns**: The current design couples two independent concerns into a single state variable: (a) whether the proxy can accept new requests, and (b) whether upstream responses are still in-flight and need to be drained. These should be decoupled.

4. **No "draining" state or flag**: The code lacks a mechanism to indicate "winner sent, accepting new requests, but still expecting late responses that should be logged and discarded."

## Correctness Properties

Property 1: Bug Condition — New requests accepted after winner sent

_For any_ non-broadcast race where a winner has been found and the winning response has been sent to the client, the proxy SHALL immediately transition to a state that accepts new client requests, regardless of how many upstream responses are still pending.

**Validates: Requirements 2.1, 2.2**

Property 2: Preservation — Broadcast races still block until all complete

_For any_ broadcast race (`submitblock`, `sendrawtransaction`) where `all_must_complete` is true, the proxy SHALL continue to block new client requests until all upstream nodes have responded (or timed out), preserving the existing broadcast completion semantics.

**Validates: Requirements 3.1, 3.2**

Property 3: Preservation — Late responses still logged and drained

_For any_ late upstream response arriving after the winner has been sent and the proxy has transitioned to accept new requests, the proxy SHALL still receive the response, log its timing information, and discard it without affecting the new request processing.

**Validates: Requirements 2.3, 3.3**

## Fix Implementation

### Changes Required

Assuming our root cause analysis is correct:

**File**: `src/rpc_proxy.c`

**Function**: `on_upstream_response()`

**Specific Changes**:

1. **Early state transition after winner sent (non-broadcast)**: After the winning response is sent to the client in the non-broadcast path, immediately set `proxy->state = RACE_IDLE` and cancel the RPC timeout timer. This allows new requests to be accepted.

2. **Track draining state separately**: Add a `bool draining` field to `rpc_proxy_t` that indicates late responses are still expected. Set it to `true` when transitioning to IDLE early. Late responses check this flag to know they should be logged and discarded.

3. **Handle late responses during draining**: When `on_upstream_response()` fires and `proxy->state == RACE_IDLE` but `draining == true`, decrement a `drain_pending` counter, log the response, and when the counter reaches zero, call a lightweight drain-complete function that resets only the drained connections (not the connections involved in any new active race).

4. **Handle new race while draining**: If a new request arrives and starts a new race while draining is still active, the new dispatch must skip upstream connections that are still in `CONN_RECEIVING` state from the previous race (they are already skipped because `dispatch_fanout` checks `conn->state != CONN_CONNECTED`). When those late responses finally arrive, they are attributed to the drain, not the new race.

5. **Drain-complete cleanup**: When `drain_pending` reaches zero (or the timeout fires for the old race), reset the drained connections via `rpc_conn_reset()`. If a new race is already active, only reset connections not participating in the new race.

**Alternative simpler approach** (preferred if safe):

Rather than a full draining subsystem, the simplest correct fix is:

1. After sending the winning response in a non-broadcast race, immediately set `proxy->state = RACE_IDLE` and cancel the timeout.
2. Keep `responses_pending` as-is for the drain count.
3. When `on_upstream_response()` fires and `proxy->state == RACE_IDLE` (meaning we're in drain mode from a previous race), log the late response and call `rpc_conn_reset()` on that individual connection. Do NOT call `race_complete()` (the race is already logically complete).
4. When `on_upstream_error()` fires and `proxy->state == RACE_IDLE`, similarly just reset that connection.
5. The existing `dispatch_fanout()` already skips connections not in `CONN_CONNECTED` state, so connections still draining won't be dispatched to in a new race.

This simpler approach works because:
- `dispatch_fanout()` checks `conn->state != CONN_CONNECTED` and skips busy connections
- Each connection's `on_response` callback already identifies itself via `conn->node_index`
- `rpc_conn_reset()` is safe to call on individual connections independently

## Testing Strategy

### Validation Approach

The testing strategy follows a two-phase approach: first, surface counterexamples that demonstrate the bug on unfixed code, then verify the fix works correctly and preserves existing behavior.

### Exploratory Bug Condition Checking

**Goal**: Surface counterexamples that demonstrate the bug BEFORE implementing the fix. Confirm or refute the root cause analysis. If we refute, we will need to re-hypothesize.

**Test Plan**: Write a test that simulates a non-broadcast race with multiple upstream nodes where one responds quickly and others are delayed. Then simulate a new client request arriving before the slow nodes respond. Run on UNFIXED code to observe the request being dropped.

**Test Cases**:
1. **validateaddress race with late node**: Dispatch to 2 nodes, node[0] responds immediately, simulate new request arriving while node[1] is pending (will fail on unfixed code — request dropped)
2. **GBT height-match with late nodes**: Dispatch to 3 nodes, node[1] matches height immediately, simulate new request while node[0] and node[2] pending (will fail on unfixed code)
3. **Client reconnect after winner**: Winner sent, client disconnects and reconnects with new request while nodes pending (will fail on unfixed code)
4. **Rapid sequential requests**: Send request, get winner, immediately send next request (will fail on unfixed code if any node is slow)

**Expected Counterexamples**:
- New request arrives with `proxy->state == RACE_FANOUT` and `proxy->winner_idx != -1` → request dropped
- Log message: "Request received while race/sticky active (state=1) — dropping request"

### Fix Checking

**Goal**: Verify that for all inputs where the bug condition holds, the fixed function produces the expected behavior.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := on_upstream_response_fixed(input)
  ASSERT proxy.state == RACE_IDLE after winner sent
  ASSERT new_request_accepted == true
  ASSERT late_responses_still_logged == true
END FOR
```

### Preservation Checking

**Goal**: Verify that for all inputs where the bug condition does NOT hold, the fixed function produces the same result as the original function.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT on_upstream_response_original(input) = on_upstream_response_fixed(input)
END FOR
```

**Testing Approach**: Property-based testing is recommended for preservation checking because:
- It generates many test cases automatically across the input domain
- It catches edge cases that manual unit tests might miss
- It provides strong guarantees that behavior is unchanged for all non-buggy inputs

**Test Plan**: Observe behavior on UNFIXED code first for broadcast races, sticky requests, and all-error scenarios, then write property-based tests capturing that behavior.

**Test Cases**:
1. **Broadcast race preservation**: Verify `submitblock` and `sendrawtransaction` still block until all nodes respond — observe on unfixed code (works correctly), then verify after fix
2. **Sticky request preservation**: Verify sticky `getblocktemplate` and `preciousblock` still block until response — observe on unfixed code, then verify after fix
3. **All-error fallback preservation**: Verify that when all nodes return errors, the last error is forwarded to client — observe on unfixed code, then verify after fix
4. **Timeout preservation**: Verify RPC timeout still fires and handles unresponsive nodes correctly after fix

### Unit Tests

- Test that `proxy->state` transitions to `RACE_IDLE` immediately after non-broadcast winner is sent
- Test that late responses arriving after early IDLE transition are logged and connections reset
- Test that broadcast races (`all_must_complete=true`) do NOT transition early
- Test that a new race dispatched while draining skips still-receiving connections
- Test that `on_upstream_error()` during drain mode resets the individual connection

### Property-Based Tests

- Generate random sequences of {race_type, node_count, winner_index, response_order, new_request_timing} and verify:
  - Non-broadcast races with winner always accept new requests immediately
  - Broadcast races always block until all complete
  - Late responses are always logged regardless of new request state
- Generate random interleaving of responses and new requests to verify no state corruption

### Integration Tests

- Full proxy lifecycle: connect client, send `validateaddress`, get response, immediately send `getblocktemplate` — verify both succeed without timeout
- Simulate slow node (delayed response) with rapid client requests — verify no drops
- Verify GBT race timing logs still appear for all nodes even after early IDLE transition
- Verify `submitblock` broadcast still waits for all nodes before accepting next request
