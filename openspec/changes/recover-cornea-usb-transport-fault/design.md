## Context

See `proposal.md` for motivation and `specs/cornea-command-recovery/spec.md` for the behavior contract. The driver owns Python panel subprocesses whose lower-level blocking limits can reach 60 seconds for Power On and 30 seconds for Power Off, while the existing upstream flow timeout is 15 seconds. A failed subprocess can be removed from the bridge while controller/UI caches still report powered-on, and only brightness has a reconnect-once path. Six stations must remain independent: recovery of one serial must not unnecessarily block healthy serials.

The 2026-08-31 LEA logs contain 280 Power On and 129 Power Off requests with no timeout or unmatched response. Maximum observed end-to-end times were 10.938 seconds and 9.395 seconds; driver RX-to-TX maxima were 10.340 seconds and 3.245 seconds. These measurements support retaining the existing 15-second client timeout and do not support extending it to 65/35 seconds.

The repository is currently on `master`, 27 commits ahead of `origin/master`, with existing tracked and untracked work in several affected modules. Implementation must preserve and integrate with that work rather than reset, stash, or replace it.

## Goals / Non-Goals

**Goals:**

- Centralize transient-fault handling so Power On/Off, brightness, and image commands use one per-serial recovery policy.
- Bound every Power operation at the driver below the unchanged 15-second client timeout.
- Make request and session identity sufficient to reject late results deterministically.
- Keep the other five panels responsive while one serial is being recovered.
- Make UI cleanup independent of which asynchronous exit path occurs.

**Non-Goals:**

- Modify qiutai source code or its existing flow timeout.
- Automatically repair missing calibration files, incorrect UCID mappings, physical cabling, power-supply, or hub failures.
- Retry terminal failures or guarantee recovery when the operating system never re-enumerates the expected serial.
- Replace the existing TCP JSON protocol or require deployed clients to add `requestId`.

## Decisions

### 1. Use a per-serial recovery coordinator and monotonic generation

Each serial has one coordinator with `Ready`, `Recovering`, and `Unavailable` states plus a monotonically increasing generation. A transport or worker fault atomically invalidates the generation, prevents new hardware commands for that serial, stops and reaps the old worker within a bound, rediscovers the exact expected serial, creates a new worker, and performs full initialization. Only then may one eligible interrupted command be retried. Commands received while recovery is active return `RECOVERY_IN_PROGRESS` rather than entering an unbounded queue.

Per-serial coordination is chosen over a global recovery lock so one faulty panel does not stall the remaining stations. Existing optional global USB serialization remains available for deployments that require it, but it is not the recovery-state source.

Alternative considered: duplicate disconnect/connect retry logic inside each command. Rejected because it produces inconsistent retry rules, stale caches, and overlapping recovery attempts.

### 2. Separate the 14-second command deadline from worker recovery

The TCP/UI command context contains request ID, serial, command, a configurable 14-second initial deadline, active state, and captured session generation. At that deadline the driver returns or presents `COMMAND_TIMEOUT`, marks power state Unknown, invalidates the request, clears UI busy state, and begins or continues bounded worker termination. The serial remains Recovering or Unavailable until the old worker is reaped and a replacement is verified.

USB rediscovery and worker recreation use a separate bounded recovery budget. Recovery may retry the interrupted operation once only if the retry can still finish before the original command deadline. Otherwise the original request completes with a structured error while recovery continues for future commands. A late recovery or retry result cannot change the expired request or UI state.

Alternative considered: extend the upstream timeout to 65/35 seconds. Rejected because all 409 measured requests completed within 15 seconds and the extension would make genuine transport stalls slower to detect.

### 3. Correlate results with request ID and session generation

The driver echoes a caller-provided `requestId`; when absent it generates an identifier for internal correlation and may add it to the response without changing existing fields. Completion publication requires the request to remain active and its captured generation to equal the current generation. Existing per-call sockets remain, but socket separation is not sufficient protection for driver-internal callbacks and cached state.

Alternative considered: correlate by command plus serial or FIFO order. Rejected because consecutive requests cannot distinguish a late completion.

### 4. Introduce explicit On, Off, and Unknown power states

Verified current-generation Power success produces On or Off. Transport faults, timeouts, worker exit, disconnect without a verified transition, and a freshly initialized replacement produce Unknown. `connected` means a live initialized worker bound to the expected serial; `commandError` is presentation derived from the current outcome, not a hardware source of truth. Status responses add `deviceState` without removing fields.

For compatibility, the existing boolean accessor remains: `isPoweredOn()` is true only for On. Callers that must distinguish Off from Unknown use the new state accessor.

Alternative considered: reset the cached boolean to false on errors. Rejected because false would incorrectly claim a disconnected or timed-out panel is physically Off.

### 5. Make UI operation completion single-owner and unconditional

Each manual Power operation obtains a scoped token. Success, failure, exception, timeout, disconnect, and panel destruction converge on one UI-thread completion method that consumes the token exactly once, clears busy state, applies only a current-generation result, and refreshes controls. A watchdog uses the command deadline plus a small UI-dispatch margin so a lost callback cannot leave controls disabled.

Alternative considered: add busy-flag cleanup separately to each error branch. Rejected because future paths can repeat the omission and old callbacks can arrive after replacement.

## Risks / Trade-offs

- **Killing a stuck worker may leave USB unavailable until Windows re-enumerates it** → use bounded rediscovery, report `RECOVERY_FAILED` with Unknown state, and require explicit operator or hub intervention after exhaustion.
- **A valid operation may exceed the measured 10.938-second maximum** → begin with a configurable 14-second deadline, log elapsed-time percentiles, and revisit only with multi-day evidence while remaining below the client boundary.
- **Generation checks can expose implicit ordering assumptions** → add deterministic tests for timeout, late completion, and replacement-session races.
- **Legacy clients omit request IDs** → generate internal IDs and preserve every existing response field.
- **Affected files already contain local edits** → implement narrowly against the current working tree, inspect every overlapping diff, and never reset unrelated work.

## Migration Plan

1. Add backward-compatible protocol fields and recovery/state behavior.
2. Run fault-injection and UI tests, including all six serials with one injected failure.
3. Deploy to a test station and confirm normal Power timing with the unchanged 15-second upstream timeout.
4. Run an LEA six-station soak test covering Power, brightness, and image delivery.
5. Roll back by disabling automatic recovery and restoring the prior driver build; no upstream rollback is required.

## Open Questions

- The 14-second deadline may be adjusted after collecting multi-day normal-operation P95/P99 timings, but it must remain below the unchanged 15-second client timeout unless a separate change revises that contract.
- Whether a site-supported USB hub reset API is available can be decided later; exhausted software recovery initially requests operator intervention.
