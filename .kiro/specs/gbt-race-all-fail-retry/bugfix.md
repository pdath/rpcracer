# Bugfix Requirements Document

## Introduction

When a block notify arrives, the proxy clears sticky state and the next `getblocktemplate` request triggers a fan-out race to all upstream nodes. If ALL nodes fail simultaneously (e.g., remote nodes in IBD, local node's connection being recycled), the proxy immediately returns an RPC error to the client and transitions to `RACE_IDLE`. The stratum proxy (ckpool) then waits its own timeout (60s) before retrying, causing unacceptable GBT response latency.

The root cause is that `on_upstream_error` and `dispatch_fanout` have no retry path for the critical post-notify GBT race. The proxy's reconnect timer already recovers connections within 1-2 seconds, but the failed race result is already sent before reconnection completes.

## Bug Analysis

### Current Behavior (Defect)

1.1 WHEN a block notify arrives AND the subsequent GBT race fires AND all upstream nodes fail simultaneously (connection errors or IBD) THEN the system immediately sends an RPC error response to the client and transitions to RACE_IDLE with no retry attempt

1.2 WHEN all nodes fail during a post-notify GBT race AND a node reconnects within 1-2 seconds THEN the system does not leverage the reconnection to retry the failed GBT request, requiring the client to initiate a new request from scratch

1.3 WHEN dispatch_fanout returns 0 (no nodes in CONN_CONNECTED state at dispatch time) for a post-notify GBT race THEN the system immediately returns "All upstream nodes unreachable" without waiting for any node to become available

### Expected Behavior (Correct)

2.1 WHEN a block notify arrives AND the subsequent GBT race fires AND all upstream nodes fail simultaneously THEN the system SHALL enter a bounded retry loop, waiting for at least one node to reconnect before retrying the GBT request

2.2 WHEN all nodes fail during a post-notify GBT race AND a node reconnects within the retry window THEN the system SHALL automatically re-dispatch the GBT request to the reconnected node without requiring a new client request

2.3 WHEN dispatch_fanout returns 0 for a post-notify GBT race THEN the system SHALL wait up to a configurable retry timeout for a node to reach CONN_CONNECTED state and then re-dispatch, rather than failing immediately

2.4 WHEN the retry timeout expires without any node reconnecting THEN the system SHALL send the RPC error to the client (same behavior as today, but only after exhausting retry attempts)

2.5 WHEN a retry succeeds (node reconnects and GBT response is received) THEN the system SHALL send the GBT response to the client normally, set sticky state, and transition to RACE_IDLE as if the original race had succeeded

### Unchanged Behavior (Regression Prevention)

3.1 WHEN a non-GBT race method (validateaddress, decoderawtransaction) has all nodes fail THEN the system SHALL CONTINUE TO return the error immediately without retry (retry only applies to post-notify GBT)

3.2 WHEN a GBT race succeeds on the first attempt (at least one node responds successfully) THEN the system SHALL CONTINUE TO select the winner by height matching and respond to the client immediately

3.3 WHEN a sticky GBT request fails (single node, no race) THEN the system SHALL CONTINUE TO fall back to fan-out race as it does today

3.4 WHEN submitblock or sendrawtransaction is broadcast and all nodes fail THEN the system SHALL CONTINUE TO return the error immediately (retry only applies to post-notify GBT)

3.5 WHEN the RPC timeout fires during a normal (non-retry) race that has pending responses THEN the system SHALL CONTINUE TO send a timeout error and call race_complete as it does today

3.6 WHEN a block notify arrives during a retry-in-progress THEN the system SHALL CONTINUE TO honor the new notify by clearing sticky state (the retry should still complete for the current block)
