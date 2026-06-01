# Bugfix Requirements Document

## Introduction

After a non-broadcast race (e.g., `validateaddress`) finds a winner, the RPC proxy remains in `RACE_FANOUT` state waiting for remaining upstream nodes to respond. Any new client request arriving during this window is silently dropped with a warning log, causing the downstream client (ckpool) to time out. The proxy must transition to an idle-like state immediately after sending the winning response so it can accept new requests, while still allowing late upstream responses to arrive and be logged.

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN a non-broadcast race has a winner and responses are still pending from other nodes THEN the system remains in `RACE_FANOUT` state and drops any new client request with the warning "Request received while race/sticky active (state=1) — dropping request"

1.2 WHEN the client disconnects and reconnects with a new request while the proxy is still in `RACE_FANOUT` waiting for slow nodes THEN the system drops the new request silently, causing the client to time out

1.3 WHEN a non-broadcast race winner is found THEN the system waits up to `rpc_timeout_ms` for all remaining nodes to respond before transitioning back to `RACE_IDLE`

### Expected Behavior (Correct)

2.1 WHEN a non-broadcast race has a winner and the winning response has been sent to the client THEN the system SHALL immediately transition to a state that accepts new client requests without waiting for remaining upstream responses

2.2 WHEN the client disconnects and reconnects with a new request after a race winner was already sent THEN the system SHALL accept and dispatch the new request normally

2.3 WHEN late responses arrive from upstream nodes after the race winner has been sent THEN the system SHALL still receive and log those responses (including `elapsed_us` and `since_notify_us` for GBT) before discarding them, without blocking new request processing

### Unchanged Behavior (Regression Prevention)

3.1 WHEN a broadcast race (`submitblock`, `sendrawtransaction`) is in progress THEN the system SHALL CONTINUE TO wait for all nodes to respond before accepting new requests

3.2 WHEN all upstream nodes respond before a new client request arrives THEN the system SHALL CONTINUE TO call `race_complete()` and reset all connections normally

3.3 WHEN a GBT race finds a height-match winner THEN the system SHALL CONTINUE TO log per-node response timing (`elapsed_us`, `since_notify_us`) for all responding nodes

3.4 WHEN a non-broadcast race has no winner yet (all responses so far are errors) THEN the system SHALL CONTINUE TO wait for remaining nodes to respond

3.5 WHEN the RPC timeout fires THEN the system SHALL CONTINUE TO treat all pending nodes as timed out and call `race_complete()`

3.6 WHEN a sticky request is in progress THEN the system SHALL CONTINUE TO block new requests until the sticky response arrives
