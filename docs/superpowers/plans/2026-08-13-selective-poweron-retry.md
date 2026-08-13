# Selective Power-on Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fail deterministic missing-panel initialization after one attempt while retaining one retry for transient worker or USB failures, with log-only diagnostics and version 1.1.27.

**Architecture:** The Python worker classifies vendor initialization results in its private subprocess reply. PythonBridge consumes that classification to control its existing two-attempt loop and logs the decision; TcpServer remains unchanged.

**Tech Stack:** Python 3.12 unittest, C++17, Qt 5.15, MSVC nmake.

## Global Constraints

- Do not change the public TCP response schema.
- Do not retry deterministic `init_ok=false`, I2C NACK, or power-sequencing failures.
- Retry transient worker/USB failures at most once.
- Application version must be exactly `1.1.27`.

---

### Task 1: Worker failure classification

**Files:**
- Modify: `python/panel_worker.py`
- Test: `python/test_panel_worker_defect_maps.py`

**Interfaces:**
- Produces: internal init data keys `failure_kind: str`, `failure_reason: str`, and `retryable: bool` when `init_ok` is false.

- [ ] Add a failing worker test whose fake vendor object logs `NACK @ 0x20` and returns `init_ok=false`; assert the init result is non-retryable hardware failure.
- [ ] Run `python -m unittest python/test_panel_worker_defect_maps.py -v` and confirm the new assertion fails because classification fields are absent.
- [ ] Capture relevant vendor error logs during construction and return the minimal private classification fields.
- [ ] Re-run the worker test and confirm it passes.

### Task 2: Selective C++ retry loop

**Files:**
- Modify: `src/pythonbridge.cpp`
- Test: `tests/test_multipanel_worker_architecture.py`

**Interfaces:**
- Consumes: `retryable`, `failure_kind`, and `failure_reason` from Task 1.

- [ ] Add failing architecture regressions that require a deterministic-failure `break`, retain `continue` for transient init failures, and forbid a final-attempt sleep.
- [ ] Run the targeted unittest and confirm failure against the unconditional retry loop.
- [ ] Read the internal classification, log it, stop after deterministic failure, and sleep only when another retry will occur.
- [ ] Run the targeted test and the complete architecture suite.

### Task 3: Version and full verification

**Files:**
- Modify: `src/main.cpp`

- [ ] Change application version from `1.1.26` to `1.1.27`.
- [ ] Run all relevant Python unit tests.
- [ ] Run a clean MSVC Release qmake+nmake build.
- [ ] Inspect the final diff to confirm TcpServer and TCP documentation are unchanged by this task.

