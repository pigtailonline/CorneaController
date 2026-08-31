## Why

LEA operation can leave a Cornea panel unusable after `USB_TRANSPORT_LOST`: the panel worker is stopped, the serial temporarily disappears, and Power On/Off can remain disabled until the entire driver application is restarted. The 2026-08-31 field logs show normal Power commands complete within the existing 15-second client timeout, so the fix belongs in this Cornea driver rather than in a longer upstream timeout.

## What Changes

- Add bounded, per-serial recovery for transient Cornea USB transport and worker failures: quarantine the failed session, rebuild it only after the expected serial is rediscovered and initialized, then retry an eligible command at most once.
- Add a driver-owned Power On/Off deadline shorter than the existing 15-second client timeout and return structured timeout responses containing `errorCode`, `requestId`, `timeout_ms`, and an `unknown` device state.
- Keep USB recovery on a separate bounded budget; recovery and at-most-once retry may complete the original command only while its deadline remains, otherwise the driver responds and recovery continues without holding the TCP caller or UI busy state.
- Prevent late responses from an expired worker generation or request from updating a newer command or UI state.
- Replace the cached Power state assumption with explicit `On`, `Off`, and `Unknown` states after transport faults and timeouts.
- Guarantee that every Power On/Off completion path, including exception, timeout, disconnect, and recovery failure, releases the UI busy state.
- Preserve backward compatibility for clients that do not send `requestId`; existing commands and response fields remain valid.
- Treat non-transient failures such as missing calibration data and initialization rejection as terminal errors rather than retry candidates.

## Capabilities

### New Capabilities

- `cornea-command-recovery`: Reliable Cornea Power and USB command execution in the driver, including bounded recovery, structured timeout responses, late-result suppression, and truthful device/UI state.

### Modified Capabilities

None.

## Impact

- TCP command dispatch, Python worker lifecycle, per-serial recovery, Power On/Off timeout responses, controller state, and device-control-panel UI handling.
- TCP JSON responses gain optional correlation and error fields while preserving existing commands and response fields.
- The upstream client and its existing 15-second flow timeout remain unchanged; the driver response deadline stays below that boundary.
- Automated fault-injection and contract tests cover USB transport loss, serial rediscovery failure, worker timeout, late completion, and UI busy-state cleanup.
