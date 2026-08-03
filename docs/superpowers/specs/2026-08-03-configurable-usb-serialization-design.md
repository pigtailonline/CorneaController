# Configurable USB Serialization

## Goal

Allow different FTDI/Cornea devices to execute concurrently by default while
retaining an operator-controlled fallback to the current global USB
serialization behavior.

The change targets multi-panel throughput. It must not remove per-panel command
ordering, serial-number validation, power failure reporting, subprocess
isolation, or USB error codes.

## Configuration

Add `serialize_usb` to the existing `python` configuration object:

```json
{
  "python": {
    "serialize_usb": false
  }
}
```

Rules:

- The compiled default is `false`.
- A missing key is interpreted as `false`, including existing station config
  files created by older releases.
- Saving a configuration writes the key explicitly.
- Setting the key to `true` restores global serialization without requiring a
  different executable.
- Startup logs state whether USB serialization is enabled or disabled.

## Lock Behavior

When `serialize_usb` is `false`:

- TCP commands do not take the process-wide `QReadWriteLock`.
- Subprocess calls do not take the C++ process-wide `LibusbGateLease`.
- Embedded Python calls do not take the process-wide libusb mutex.
- `panel_worker.py` does not take the named Windows USB mutex.
- Commands for one panel remain ordered by that panel's dedicated
  `PanelSubprocess` worker thread.
- Commands for different panels may run concurrently.

When `serialize_usb` is `true`, the existing global lock behavior remains:

- `powerOn` and `powerOff` are exclusive at the TCP layer.
- Other asynchronous TCP operations use the shared/read side of the lock.
- C++ subprocess and embedded Python operations use their global USB gates.
- Python workers use `Global\\CorneaController_USB_GATE_v1` as a second
  cross-process guard.

The setting controls all four layers together. Partial states, such as disabling
the TCP lock while leaving the worker gate enabled, are not supported because
they appear concurrent to callers while remaining serialized internally.

## Propagation

`PythonConfig` owns the parsed value. During application setup,
`CorneaWidget` passes it to `PythonBridge` and `TcpServer`.

`PythonBridge` uses the value for embedded calls and its subprocess gate. It
also adds `CORNEA_SERIALIZE_USB=0|1` to each `PanelSubprocess` environment before
the child starts. `panel_worker.py` reads the environment once during startup
and conditionally uses its named mutex.

No TCP API field is added. The mode is station configuration, not a per-command
request option, so all callers observe one consistent policy during a process
lifetime.

## Error Handling

Existing command responses remain unchanged. A panel power failure must still
return `success:false` for that serial, independent of the serialization mode.

When serialization is disabled, errors from one panel must not stop another
panel's worker. Existing serial verification and unhealthy-worker isolation
remain active. `USB_BUSY_TIMEOUT` is expected only when serialization is
enabled; transport failures continue to use `USB_TRANSPORT_LOST`.

## Compatibility

- Existing configs automatically select concurrent mode because the missing
  key defaults to `false`, as requested.
- Setting `serialize_usb:true` provides the production fallback for stations
  that reproduce the historical libusb-win32 multi-board hang.
- The existing `use_subprocess` default and worker architecture are unchanged.
- This change only permits concurrency. The upper-level DSA controller must
  issue multiple `powerOn` requests concurrently to reduce two-panel wall-clock
  time to approximately one panel's initialization time.

## Tests

Automated tests will cover:

1. Missing `serialize_usb` parses as `false`.
2. Explicit `true` and `false` values parse and save correctly.
3. Startup wiring passes the selected mode to `TcpServer`, `PythonBridge`, and
   the worker environment.
4. Default mode bypasses each global lock while preserving per-panel worker
   ordering.
5. Enabled mode retains current global serialization and timeout behavior.
6. TCP `powerOn` success and failure responses remain associated with the
   requested serial.

Build verification will run the existing Python architecture tests and the Qt
release build. Hardware validation should use two known-good panels for at
least 100 concurrent cold-start cycles, recording hangs, NACKs, serial
mismatches, transport errors, and total wall-clock time.

## Non-Goals

- Changing Cornea/RJ1 power sequencing.
- Treating I2C `0x20` or `0x30` NACK as success.
- Removing subprocess isolation or serial-number re-resolution.
- Modifying the upper-level DSA application's dispatch order in this repository.
