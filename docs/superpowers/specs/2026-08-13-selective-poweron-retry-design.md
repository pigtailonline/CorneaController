# Selective power-on retry design

## Objective

Reduce failed `powerOn` latency by avoiding a second complete initialization when the first attempt already proves a deterministic hardware failure. Preserve one retry for transient worker and USB failures. Bump CorneaController from 1.1.26 to 1.1.27.

## Scope

- Classify failed worker initialization as retryable or deterministic.
- Treat `init_ok=false`, power-sequencing failure, and I2C NACK evidence such as address `0x20` as deterministic.
- Retry worker startup, worker communication, unstable enumeration, and transient USB transport failures at most once.
- Record the classification, retry decision, and reason in the driver log.
- Keep the public TCP response schema unchanged.
- Remove the unconditional one-second delay after the final failed attempt.

## Design

`panel_worker.py` captures initialization errors emitted by the vendor library and includes private `failure_kind`, `failure_reason`, and `retryable` fields in the worker-to-parent JSON data. These fields are internal subprocess protocol details, not TCP API fields.

`PythonBridge::createDeviceInstance` reads the internal fields. A deterministic failure stops the failed worker and exits the retry loop immediately. A retryable worker/USB failure restarts the worker once. Every decision is logged with the attempt number, classification, retry flag, and reason.

## Verification

- A regression test proves deterministic `init_ok=false` exits after one attempt.
- A regression test proves transient init failures retain the second attempt.
- Worker tests prove power-sequencing/NACK failures are classified as non-retryable.
- Existing Python suites and a clean MSVC Release build must pass.

