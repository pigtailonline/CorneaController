# Command Result Validation Design

## Goal

Make TCP command results reflect the hardware operation that was requested while
keeping the existing power-on success contract. A successful `powerOn` remains
successful when `init_ok=true`; failures from `setBrightness`, `sendImage`, and
`getTemperature` must no longer be reported as `success:true`.

This change is included in Cornea Controller `1.1.26` and does not introduce an
automatic power-off for these three command failures.

## Observed False Positives

1. `panel_worker.py` calls `set_brightness()` but ignores a literal `False`
   return and does not compare the requested value with `get_brightness()`.
2. `panel_worker.py` returns `{"ok": false}` when `write_rj1_frame()` fails,
   but `PythonBridge::writeFrame()` checks only the JSON-RPC envelope's
   `success` field.
3. A temperature such as `-40.0 C` passes the current `< -900` sentinel check
   and is returned to the TCP client as a successful measurement.

## Command Semantics

### Power On

`powerOn` continues to use `init_ok` as its success criterion. If the driver
logs a critical power-sequencing or I2C NACK message while still returning
`init_ok=true`, the response remains successful and may carry warning details.
The panel remains powered.

The safe-brightness call made by the TCP power-on handler is not allowed to
silently disappear. If it fails, the `powerOn` response still remains
`success:true`, with a `BRIGHTNESS_SET_FAILED` warning because power-on itself
succeeded.

### Set Brightness

`setBrightness` returns `success:false` with error code
`BRIGHTNESS_SET_FAILED` when any of these conditions occurs:

- `set_brightness()` throws.
- `set_brightness()` returns the literal boolean `False`. A `None` return is
  not treated as failure because setter-style driver APIs may return no value.
- `get_brightness()` throws after the write.
- The requested and read-back brightness values differ after rounding to two
  decimal places, matching the application's UI precision.

The failure is semantic, not a transport loss. The worker is not isolated and
the controller does not reconnect or power-cycle the panel automatically.

### Send Image

`sendImage` returns `success:false` with error code `IMAGE_TRANSFER_FAILED`
when `write_rj1_frame()` returns a false value. The C++ bridge checks both the
JSON-RPC envelope and the returned `data.ok` field so this cannot regress if
the Python response shape is changed later.

This semantic failure does not mark the subprocess unhealthy and does not
automatically power off the panel. Exceptions and transport failures retain
their existing recovery behavior.

### Get Temperature

RJ1 temperatures at or below `-30.0 C`, including the observed `-40.0 C`, are
invalid for this production station. Non-finite values and the existing
`-999.0` sentinel are also invalid. `getTemperature` returns `success:false`
with error code `INVALID_TEMPERATURE` and includes the rejected value in the
error message.

The raw value remains available inside the controller/UI path so the local UI
can display `Invalid (-40.0 C)` rather than hiding the symptom as `-`.

## TCP Error Shape

Failures use the existing response envelope:

```json
{
  "success": false,
  "errorCode": "INVALID_TEMPERATURE",
  "error": "INVALID_TEMPERATURE: invalid RJ1 temperature -40.0 C",
  "cmd": "getTemperature",
  "serial": "LITE20240380"
}
```

`TcpServer::makeError()` recognizes these additional stable codes:

- `BRIGHTNESS_SET_FAILED`
- `IMAGE_TRANSFER_FAILED`
- `INVALID_TEMPERATURE`

## Local UI Feedback

The power state and command result are displayed separately:

- Powered and healthy: green `On`.
- Powered after one of the three command failures: orange
  `On - Command Error`.
- Invalid RJ1 temperature: red `Invalid (<value> C)`.
- Powered off: gray `Off`, with the latched command-error state cleared.

The command-error state is latched so an operator cannot miss a transient
failure. It clears on power-off or at the start of the next power-on cycle. No
modal dialog is added, so automated TCP station operation is not blocked.

## Data Flow

1. `panel_worker.py` preserves the driver's literal operation result and
   brightness read-back in structured response data.
2. `PythonBridge` validates structured results and stores a stable error code
   in `m_lastError` when an operation is semantically unsuccessful.
3. `CorneaController` emits its existing `errorOccurred` signal for failed
   brightness/image operations while leaving `m_poweredOn` unchanged.
4. `CorneaWidget` and `TcpServer` propagate the detailed error instead of
   replacing it with a generic message.
5. `DeviceControlPanel` latches the command-error state and updates its status
   and temperature labels on the GUI thread.

## Compatibility

- Command names and required parameters do not change.
- `powerOn` success behavior remains compatible.
- Existing clients already treating `success:false` as command failure need
  no protocol change.
- No station configuration key is added.
- Existing APL and over-temperature shutdown behavior is unchanged.

## Verification

Automated tests cover:

- Literal false and read-back mismatch from `set_brightness()`.
- `write_rj1_frame()` returning false.
- `-40.0 C`, the legacy sentinel, and valid room-temperature samples.
- Stable TCP error-code propagation.
- Power-on remaining successful when its post-power safe-brightness step
  reports a warning.
- Orange powered-on command-error state and red invalid-temperature display.
- Existing full Python suite and Qt Release compilation.

Station validation should run the four-command sequence shown in the field
report and confirm that only `powerOn` remains successful when the other
operations return their simulated failure values.
