# Implementation Plan: ck-notify-socket

## Overview

Add an optional CK socket relay to the downstream notification pipeline. The implementation extends `config_t` with a socket path field, adds relay state to the `notifier` struct, and implements two static functions (`ck_connect` and `relay_ck_notify`) in `notifier.c`. The relay sends a fixed 10-byte length-prefixed "update" message over a Unix domain socket using non-blocking I/O with reconnect-on-demand semantics.

## Tasks

- [x] 1. Extend configuration to support `ck_notify_socket`
  - [x] 1.1 Add `ck_notify_socket[108]` field to `config_t` in `src/config.h`
    - Add `char ck_notify_socket[108];` to the downstream notification relay section of `config_t`
    - Place it after `notify_http_url[512]`
    - _Requirements: 1.1, 1.2_

  - [x] 1.2 Implement parsing of `ck_notify_socket` in `src/config.c`
    - In `config_load()`, after existing field parsing, read the `"ck_notify_socket"` JSON key
    - If absent or empty string: set first byte to NUL (relay disabled)
    - If string of length 1–107: copy into `cfg->ck_notify_socket`
    - If string length > 107: log error with path length limit, return NULL
    - If present but not a string type: log error with invalid type message, return NULL
    - Update the "no downstream method" warning to include `ck_notify_socket[0] == '\0'` in the condition
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 6.5_

  - [x] 1.3 Write unit tests for `ck_notify_socket` config parsing (`tests/test_config_ck_socket.c`)
    - Test: valid path (e.g. `/tmp/ckpool/stratifier`) stored correctly
    - Test: absent field → empty string
    - Test: empty string value → empty string
    - Test: path of exactly 107 chars → accepted
    - Test: path of 108+ chars → `config_load` returns NULL
    - Test: non-string value (integer, boolean, null, array, object) → `config_load` returns NULL
    - _Requirements: 1.1, 1.2, 1.3, 1.4_

  - [x] 1.4 Write property test for valid socket path round-trip (`tests/test_ck_path_roundtrip.c`)
    - **Property 1: Valid socket path round-trip**
    - Generate random strings of length 1–107 using valid path characters (printable ASCII excluding NUL)
    - Serialize as JSON config with `ck_notify_socket` field, parse with `config_load`
    - Assert parsed `ck_notify_socket` equals original string byte-for-byte
    - Use seeded `srand48`/`lrand48`, 500 trials, seed printed and accepted via `argv[1]`
    - **Validates: Requirements 1.1**

  - [x] 1.5 Write property test for over-length path rejection (`tests/test_ck_path_overlen.c`)
    - **Property 2: Over-length path rejection**
    - Generate random strings of length 108–512
    - Serialize as JSON config, parse with `config_load`
    - Assert `config_load` returns NULL for every trial
    - 200 trials, seeded PRNG
    - **Validates: Requirements 1.3**

  - [x] 1.6 Write property test for non-string type rejection (`tests/test_ck_path_type.c`)
    - **Property 3: Non-string type rejection**
    - Generate random JSON values of types: integer, boolean, null, array, object
    - Write raw JSON config file with `ck_notify_socket` set to the generated value
    - Assert `config_load` returns NULL for every trial
    - 100 trials, seeded PRNG
    - **Validates: Requirements 1.4**

- [x] 2. Checkpoint - Verify config changes compile and tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 3. Implement CK socket relay in notifier
  - [x] 3.1 Add CK relay state fields to `struct notifier` in `src/notifier.c`
    - Add `int ck_fd;` (initialized to -1)
    - Add `char ck_path[108];`
    - Add `bool ck_configured;`
    - Initialize in `notifier_create()`: copy path from `cfg->ck_notify_socket`, set `ck_configured = (ck_path[0] != '\0')`, set `ck_fd = -1`
    - _Requirements: 2.1, 2.2_

  - [x] 3.2 Implement `static int ck_connect(const char *path)` in `src/notifier.c`
    - Create `AF_UNIX, SOCK_STREAM` socket
    - Build `struct sockaddr_un` with the path
    - Call `connect()`; on failure close fd and return -1
    - Set `O_NONBLOCK` via `fcntl` (reuse existing `set_nonblocking` helper)
    - Return connected fd on success, -1 on failure
    - Log INFO on success with `[ck]` tag and path
    - _Requirements: 2.1, 2.3, 8.1_

  - [x] 3.3 Implement `static void relay_ck_notify(notifier_t *n, const uint8_t *hash)` in `src/notifier.c`
    - Early return if `!n->ck_configured`
    - Define `static const uint8_t CK_UPDATE_MSG[10]` as the 10-byte message
    - If `ck_fd == -1`: call `ck_connect(n->ck_path)`, store result in `n->ck_fd`; if still -1, log WARN with `[ck]` tag + strerror, return
    - Call `write(n->ck_fd, CK_UPDATE_MSG, 10)` (single call, no retry)
    - If write returns 10: log INFO with `[ck]` tag and hex hash
    - If write returns -1 with EAGAIN/EWOULDBLOCK: log WARN, return (stay connected)
    - If write returns -1 with other error: log WARN with strerror, close fd, set `ck_fd = -1`
    - If write returns 0 < n < 10 (partial): log WARN, close fd, set `ck_fd = -1`
    - _Requirements: 3.1, 3.2, 4.1, 4.2, 4.3, 4.4, 4.5, 5.1, 5.2, 5.3, 5.4, 8.2, 8.3, 8.4_

  - [x] 3.4 Integrate `relay_ck_notify` into `notifier_process_hash()` in `src/notifier.c`
    - Add call `relay_ck_notify(n, hash);` after the existing `relay_http_notify(n, hash);` call
    - Also add to the grace-period suppressed relay section (after `relay_http_notify` in the suppressed path)
    - _Requirements: 6.1, 6.2, 6.3, 6.4_

  - [x] 3.5 Add CK socket cleanup in `notifier_destroy()` in `src/notifier.c`
    - Before freeing the notifier, if `n->ck_fd >= 0` call `close(n->ck_fd)`
    - _Requirements: 7.1, 7.2, 7.3_

  - [x] 3.6 Attempt initial connection in `notifier_create()` in `src/notifier.c`
    - After initializing CK state, if `ck_configured`, call `ck_connect(n->ck_path)` and store in `n->ck_fd`
    - If connect fails: log WARN, continue (non-fatal)
    - _Requirements: 2.1, 2.2_

- [x] 4. Checkpoint - Verify notifier changes compile cleanly
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Add tests for CK socket relay behavior
  - [x] 5.1 Write property test for message framing invariant (`tests/test_ck_msg_framing.c`)
    - **Property 4: Message framing invariant**
    - Create a `socketpair(AF_UNIX, SOCK_STREAM, 0)` for each trial
    - Set write end to non-blocking, invoke `relay_ck_notify` (or directly write `CK_UPDATE_MSG`)
    - Read from the read end and assert exactly 10 bytes matching `{0x06,0x00,0x00,0x00,'u','p','d','a','t','e'}`
    - Vary: random block hashes (should not affect message content)
    - 200 trials, seeded PRNG
    - **Validates: Requirements 3.1, 3.2**

  - [x] 5.2 Write property test for single write per notification (`tests/test_ck_single_write.c`)
    - **Property 5: Single write per notification**
    - Use a mock fd (pipe or socketpair) with a wrapper counting `write()` calls
    - For each trial: invoke `relay_ck_notify` once, assert write was called exactly once
    - Vary: random hash values, connected vs just-reconnected state
    - 200 trials, seeded PRNG
    - **Validates: Requirements 4.1**

  - [x] 5.3 Write property test for fatal error causes disconnect (`tests/test_ck_fatal_disconnect.c`)
    - **Property 6: Fatal error causes disconnect**
    - Set up notifier with a connected fd, then close the read end (simulate broken pipe)
    - Invoke `relay_ck_notify`, assert `ck_fd` is -1 afterwards
    - Also test partial write scenario (use a pipe with limited buffer)
    - 100 trials varying error conditions
    - **Validates: Requirements 4.4, 4.5, 5.1**

  - [x] 5.4 Write property test for reconnection on demand (`tests/test_ck_reconnect.c`)
    - **Property 7: Reconnection on demand**
    - Set up notifier in disconnected state (`ck_fd = -1`, `ck_configured = true`)
    - Start a Unix listener socket, invoke `relay_ck_notify`
    - Assert `ck_fd >= 0` after the call (reconnected) and message was received
    - 50 trials with varying socket paths
    - **Validates: Requirements 5.2**

  - [x] 5.5 Write unit test for fault isolation (`tests/test_ck_fault_isolation.c`)
    - **Property 8: Fault isolation from CK relay**
    - Set up notifier with CK configured to a non-existent path (connect will fail)
    - Invoke `notifier_process_hash` and verify that ZMQ PUB and HTTP relay still execute
    - Use mock/stub approach: verify relay_zmq_pub and relay_http_notify produce their expected side effects even when CK relay fails
    - _Requirements: 6.2, 6.3_
    - **Validates: Requirements 6.2**

- [ ] 6. Update example config and documentation
  - [x] 6.1 Add `ck_notify_socket` to `deploy/rpcrace.conf.example` and `deploy/rpcrace.conf.README`
    - Add commented-out `"ck_notify_socket": "/tmp/ckpool/stratifier"` to the example config
    - Document the field in the README with path length limit and behavior description
    - _Requirements: 1.1_

- [x] 7. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties from the design document
- Unit tests validate specific examples and edge cases
- All test files follow the project convention: one file per concern, `srand48`/`lrand48` PRNG, seed via `argv[1]`, ASSERT macro
- The CK relay is self-contained within `notifier.c` (static functions), minimizing cross-file changes
- Tests that need a Unix socket for I/O testing should use `socketpair()` or temporary paths in `/tmp`

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2"] },
    { "id": 2, "tasks": ["1.3", "1.4", "1.5", "1.6"] },
    { "id": 3, "tasks": ["3.1"] },
    { "id": 4, "tasks": ["3.2", "3.6"] },
    { "id": 5, "tasks": ["3.3"] },
    { "id": 6, "tasks": ["3.4", "3.5"] },
    { "id": 7, "tasks": ["5.1", "5.2", "5.3", "5.4", "5.5"] },
    { "id": 8, "tasks": ["6.1"] }
  ]
}
```
