# Implementation Plan

## Overview

Fix the race state blocking bug where the RPC proxy stays in `RACE_FANOUT` after a non-broadcast race winner is sent, dropping subsequent client requests. The fix transitions to `RACE_IDLE` immediately after sending the winning response for non-broadcast races, while draining late upstream responses without blocking new request processing.

## Tasks

- [x] 1. Write bug condition exploration test
  - **Property 1: Bug Condition** - Non-broadcast race blocks new requests after winner sent
  - **IMPORTANT**: Write this property-based test BEFORE implementing the fix
  - **CRITICAL**: This test MUST FAIL on unfixed code — failure confirms the bug exists
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior — it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the proxy stays in RACE_FANOUT after sending a winner response
  - **Scoped PBT Approach**: Scope the property to concrete failing cases — a non-broadcast race (e.g., `validateaddress`) dispatched to N≥2 nodes where node[0] responds successfully while node[1..N-1] are still pending, then a new client request arrives
  - Create `tests/test_race_state_blocking.c` with a test harness that:
    - Sets up a mock proxy with 2+ upstream connections in RACE_FANOUT state
    - Simulates node[0] delivering a successful response (winner found, response sent)
    - Verifies `proxy->winner_idx != -1` and `proxy->responses_pending > 0` and `proxy->all_must_complete == false`
    - Simulates a new client request arriving (calls the request-gate logic)
    - Asserts the new request is ACCEPTED (proxy state allows it)
  - Bug condition from design: `isBugCondition(input)` where `proxy_state == RACE_FANOUT AND winner_sent == true AND all_must_complete == false AND responses_pending > 0 AND new_request_arrives == true`
  - Expected behavior: proxy SHALL accept the new request (state transitions to RACE_IDLE or equivalent accepting state)
  - Run test on UNFIXED code: `scp -r Makefile configure src/ include/ tests/ odroid:~/rpcrace/ && ssh odroid "cd ~/rpcrace && ./configure && make test"`
  - **EXPECTED OUTCOME**: Test FAILS (the new request is dropped because state==RACE_FANOUT) — this confirms the bug exists
  - Document counterexample: "New request arrives with proxy->state == RACE_FANOUT and proxy->winner_idx != -1 → request dropped with 'Request received while race/sticky active (state=1)'"
  - Mark task complete when test is written, deployed, run, and failure is documented
  - _Requirements: 1.1, 1.3_

- [x] 2. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** - Broadcast races and sticky requests continue to block
  - **IMPORTANT**: Follow observation-first methodology
  - **IMPORTANT**: Write these tests BEFORE implementing the fix
  - Create `tests/test_race_state_preservation.c` with property-based tests that:
  - **Observe on UNFIXED code first:**
    - Observe: `submitblock` dispatched to 3 nodes, node[0] responds → proxy stays in RACE_FANOUT (correct, all_must_complete=true)
    - Observe: `sendrawtransaction` dispatched to 2 nodes, node[0] responds → proxy stays in RACE_FANOUT (correct, all_must_complete=true)
    - Observe: sticky `getblocktemplate` dispatched to 1 node → proxy stays in RACE_STICKY until response (correct)
    - Observe: non-broadcast race where ALL nodes return errors → last error forwarded to client, race_complete() called (correct)
    - Observe: race where no winner yet (all errors so far) with responses_pending > 0 → proxy stays in RACE_FANOUT (correct)
  - **Write property-based tests capturing observed behavior:**
    - Property: For all broadcast races (all_must_complete=true), proxy SHALL remain in RACE_FANOUT and reject new requests until all nodes respond or timeout fires
    - Property: For all sticky requests, proxy SHALL remain in RACE_STICKY and reject new requests until the sticky response arrives
    - Property: For all non-broadcast races where winner_idx == -1 (no winner yet), proxy SHALL remain in RACE_FANOUT
    - Property: For all races where responses_pending reaches 0, race_complete() is called and state transitions to RACE_IDLE
  - Verify tests pass on UNFIXED code: `scp -r tests/ odroid:~/rpcrace/ && ssh odroid "cd ~/rpcrace && make test"`
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, deployed, run, and passing on unfixed code
  - _Requirements: 3.1, 3.2, 3.4, 3.5, 3.6_

- [x] 3. Fix for race state blocking after non-broadcast winner sent

  - [x] 3.1 Implement the fix in `src/rpc_proxy.c`
    - Add early state transition to RACE_IDLE after sending winning response for non-broadcast races
    - In `on_upstream_response()`, after the non-broadcast winner is sent to client (around line 1090), when `!proxy->all_must_complete && proxy->winner_idx != -1 && proxy->responses_pending > 0`:
      - Set `proxy->state = RACE_IDLE`
      - Call `cancel_rpc_timeout(proxy)` (timeout no longer needed — winner already sent)
      - Log: "Non-broadcast winner sent, transitioning to IDLE (%d responses draining)"
    - Handle late responses during drain: In `on_upstream_response()`, add early check — if `proxy->state == RACE_IDLE` and this connection has a `send_buf` set (participated in previous race), log the late response timing and call `rpc_conn_reset(conn)` on just that connection, then return early
    - Handle late errors during drain: In `on_upstream_error()`, if `proxy->state == RACE_IDLE`, log the error and call `rpc_conn_reset(conn)` on just that connection, then return early
    - Do NOT call `race_complete()` for draining responses — the race is already logically complete
    - Ensure `dispatch_fanout()` naturally skips draining connections (it already checks `conn->state != CONN_CONNECTED`)
    - All source files must use LF line endings
    - _Bug_Condition: isBugCondition(input) where proxy_state == RACE_FANOUT AND winner_sent == true AND all_must_complete == false AND responses_pending > 0_
    - _Expected_Behavior: proxy.state == RACE_IDLE after winner sent, new requests accepted immediately_
    - _Preservation: Broadcast races (all_must_complete=true) unchanged; sticky requests unchanged; late response logging preserved_
    - _Requirements: 1.1, 1.2, 1.3, 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6_

  - [x] 3.2 Verify bug condition exploration test now passes
    - **Property 1: Expected Behavior** - Non-broadcast race accepts new requests after winner sent
    - **IMPORTANT**: Re-run the SAME test from task 1 — do NOT write a new test
    - The test from task 1 encodes the expected behavior (proxy accepts new requests after winner sent)
    - When this test passes, it confirms the expected behavior is satisfied
    - Deploy and run: `scp -r Makefile configure src/ include/ tests/ odroid:~/rpcrace/ && ssh odroid "cd ~/rpcrace && ./configure && make test"`
    - **EXPECTED OUTCOME**: Test PASSES (confirms bug is fixed — proxy transitions to RACE_IDLE after non-broadcast winner)
    - _Requirements: 2.1, 2.2_

  - [x] 3.3 Verify preservation tests still pass
    - **Property 2: Preservation** - Broadcast races and sticky requests continue to block
    - **IMPORTANT**: Re-run the SAME tests from task 2 — do NOT write new tests
    - Run preservation property tests from step 2
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions — broadcast races still block, sticky still blocks, all-error fallback still works)
    - Confirm all tests still pass after fix (no regressions)
    - _Requirements: 3.1, 3.2, 3.4, 3.5, 3.6_

  - [x] 3.4 Write integration test for late response draining
    - Create `tests/test_drain_late_responses.c` that verifies:
    - Late upstream responses arriving after early IDLE transition are logged with timing info and connections are reset
    - A new race dispatched while old connections are still draining skips those connections (dispatch_fanout checks conn->state != CONN_CONNECTED)
    - Late errors during drain reset the individual connection without affecting new race state
    - GBT height logging (`elapsed_us`, `since_notify_us`) still works for all responding nodes even after early transition
    - Deploy and run: `scp -r tests/ odroid:~/rpcrace/ && ssh odroid "cd ~/rpcrace && make test"`
    - _Requirements: 2.3, 3.3_

- [x] 4. Checkpoint — Ensure all tests pass
  - Deploy full project: `scp -r Makefile configure src/ include/ tests/ deploy/ odroid:~/rpcrace/`
  - Run full test suite: `ssh odroid "cd ~/rpcrace && ./configure && make clean && make && make test"`
  - Verify all existing tests still pass (no regressions in test_race_winner, test_slow_response, test_all_error_fallback, etc.)
  - Verify new tests pass (test_race_state_blocking, test_race_state_preservation, test_drain_late_responses)
  - Ensure all tests pass, ask the user if questions arise

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1", "2"] },
    { "id": 1, "tasks": ["3.1"] },
    { "id": 2, "tasks": ["3.2", "3.3", "3.4"] },
    { "id": 3, "tasks": ["4"] }
  ]
}
```

## Notes

- All source files must use LF line endings (not CRLF)
- Build and test on remote: `ssh odroid "cd ~/rpcrace && ./configure && make && make test"`
- Deploy via: `scp -r Makefile configure src/ include/ tests/ deploy/ odroid:~/rpcrace/`
- The simpler fix approach (no separate draining struct) is preferred per design document
- `dispatch_fanout()` already skips connections not in CONN_CONNECTED state, so draining connections are naturally excluded from new races
