## 1. Baseline and Driver Protocol Contracts

- [x] 1.1 Preserve and document the repository's dirty state and exact revision before implementation, without resetting unrelated work.
  - Baseline recorded 2026-08-31 on `master`: `HEAD=b65598abaef773ba3d52337b2c87782bdbf40bef`, `origin/master=36f53b5c612c6db822344ce0df3d0fb95084eb2c` (local ahead 27). Existing tracked edits: `README.md`, `docs/TCP API.md`, `python/panel_worker.py`, `python/test_panel_worker_defect_maps.py`, `src/corneacontroller.h`, `src/corneawidget.cpp`, `src/corneawidget.h`, `src/main.cpp`, `src/pythonbridge.cpp`, `src/pythonbridge.h`, `src/tcpserver.cpp`; existing untracked work includes `.agents/`, `HANDOFF.md`, build/repro scripts, logs, `openspec/`, and `tests/test_power_api.py`. No reset/stash/checkout performed.
- [x] 1.2 Add contract tests for legacy response compatibility, optional/generated `requestId`, structured `COMMAND_TIMEOUT`, `RECOVERY_IN_PROGRESS`, and `RECOVERY_FAILED` responses.
  - Deterministic source/response contracts are covered by `tests/test_cornea_command_recovery_contract.py` and pass in the targeted suite.
- [x] 1.3 Add deadline tests proving Power responses and manual UI completion occur within a configurable 14-second default and do not depend on lower-level 60/30-second worker waits.
  - The source contract and no-Qt deadline model verify the 14,000 ms driver deadline, the unchanged 15,000 ms client boundary, and UI watchdog completion.

## 2. Per-Serial Recovery

- [x] 2.1 Introduce a per-serial session coordinator with Ready, Recovering, and Unavailable states plus a monotonic worker generation.
- [x] 2.2 Route retryable transport/worker failures from Power On, Power Off, brightness, and image commands through the shared bounded recovery path.
- [x] 2.3 Implement bounded old-worker stop/reap, expected-serial rediscovery, new-worker initialization, and serial verification.
- [x] 2.4 Retry an eligible interrupted command at most once and only when recovery plus retry can finish before the original command deadline.
- [x] 2.5 Classify calibration, UCID/initialization rejection, argument, and configured device-operation failures as non-retryable while retaining detailed error codes.
- [x] 2.6 Reject commands targeting a Recovering or Unavailable serial with a structured response while allowing other serials to continue independently.
  - Source contracts and deterministic per-serial/generation models pass; native Windows builds succeeded, while runtime USB execution remains field validation.

## 3. Command Deadline and Late-Result Isolation

- [x] 3.1 Add an internal command context containing request ID, serial, command, deadline, active flag, and captured worker generation.
- [x] 3.2 Enforce a configurable driver-owned Power deadline with a 14-second initial default, below the unchanged 15-second client timeout.
- [x] 3.3 On deadline expiry, publish exactly one structured timeout result, set power state Unknown, release the caller/UI busy state, and quarantine the serial until cleanup or recovery completes.
- [x] 3.4 Give USB rediscovery and worker recreation a separate bounded recovery budget that never extends the expired TCP or UI operation.
- [x] 3.5 Suppress completion publication and state changes from expired requests or superseded worker generations.
- [x] 3.6 Echo a caller-provided `requestId` and generate an internal identifier for legacy requests without removing or changing existing response fields.
  - Contract tests and the executable no-Qt deadline/generation model pass; native Windows builds succeeded, while C++ runtime verification remains field validation.

## 4. Driver State and Operator UI

- [x] 4.1 Add an On/Off/Unknown power-state model while retaining a compatibility boolean accessor whose true value means On only.
- [x] 4.2 Atomically update connected, power, and command-error presentation when a session faults, recovers, disconnects, or completes a verified Power transition.
- [x] 4.3 Extend status output with `deviceState` while preserving existing fields.
- [x] 4.4 Refactor manual Power operations to use a single-owner completion token and watchdog so every success, failure, exception, timeout, disconnect, and destruction path clears busy state exactly once.
- [x] 4.5 Add UI/state tests proving `On - Command Error` cannot retain stale On state after transport loss and both Power controls become usable after every terminal outcome.
  - UI/state source contracts pass; no Qt runtime UI harness is available in this environment.

## 5. Verification and Field Readiness

- [x] 5.1 Run fault-injection tests for USB transport loss, worker exit, temporary serial absence, permanent serial absence, command-deadline expiry, and late completion after replacement.
  - Deterministic no-hardware recovery/generation tests cover transport/worker failure classification, unavailable serials, deadline expiry, and late results; physical USB injection remains field validation.
- [x] 5.2 Run a six-serial concurrency test showing one serial's recovery does not block normal commands on the other five.
  - The deterministic six-serial model verifies the recovering serial rejects work while another serial remains Ready; native multi-thread/USB stress remains unrun.
- [x] 5.3 Build and run the relevant unit, contract, and UI-state suites, recording exact commands and results.
  - Python contract/architecture/power suites: `python3 -m unittest tests.test_cornea_command_recovery_contract tests.test_multipanel_worker_architecture tests.test_power_api -v` — 67 passed.
  - Python worker suite: `python3 -m unittest discover -s python -p 'test_*.py' -v` — 11 passed.
  - Native Windows debug build: `D:\projects\src\google driver\CorneaController\build_debug_sim.bat` — qmake, moc, cl, and link succeeded; produced `build\\Desktop_Qt_5_15_2_MSVC2019_64bit-Debug\\CorneaController.exe`; final `Done. Exit code: 0`. Warnings were C4819 code-page 936 and one unused parameter; no compile/link errors.
  - Native Windows release build: `cmd.exe /C "D:/projects/src/google driver/CorneaController/build_release_full.bat"` — qmake, moc, cl, and link succeeded; produced `build\\Desktop_Qt_5_15_2_MSVC2019_64bit-Release\\release\\CorneaController.exe` (593,920 bytes); final `Done. Exit code: 0`. Warnings were empty `resources.qrc`, C4819 code-page 936, C4100 unused parameters, and LNK4099 missing `qtmain.pdb`; no compile/link errors. Post-link copied `python312.dll`, `python/panel_worker.py`, and `python/smoke_test_worker.py` into the Release output.
- [ ] 5.4 Confirm regression timings remain within the existing 15-second client timeout and compare them with the 2026-08-31 baseline of 10.938 seconds Power On and 9.395 seconds Power Off maxima.
  - The debug build cannot provide this timing smoke: `CorneaController.pro` defines `DISABLE_SIM`, so the `build_debug_sim.bat` output is not a no-hardware simulation path; a station or a separately enabled simulation build is required.
- [ ] 5.5 Perform an LEA six-station soak test covering repeated Power On/Off, brightness, and image delivery, and verify no operation remains busy beyond its configured deadline.
- [ ] 5.6 Verify the deployment has calibration data for UCID `06610050` or report the mapping/deployment issue separately from transport recovery.

> Verification blockers: 5.4 requires a runnable station timing sample (the debug build alone has no hardware timing path). 5.5 requires six physical LEA stations and controlled fault injection. 5.6 requires deployment calibration assets and a connected panel for UCID `06610050`.
