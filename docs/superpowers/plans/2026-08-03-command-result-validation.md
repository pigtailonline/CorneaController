# Command Result Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop reporting `setBrightness`, `sendImage`, and invalid temperature reads as successful while preserving the existing successful `powerOn` contract.

**Architecture:** Validate driver return values first in `panel_worker.py`, then defensively validate the structured response in `PythonBridge`. Preserve stable error codes through `CorneaController`, `CorneaWidget`, and `TcpServer`; latch a non-blocking orange command-error state in `DeviceControlPanel` while keeping the panel powered.

**Tech Stack:** Python 3.12 `unittest`, C++17, Qt 5.15, line-delimited JSON-RPC, MSVC Release build.

## Global Constraints

- Cornea Controller application version remains `1.1.26`.
- `powerOn` remains `success:true` whenever `init_ok=true`.
- `setBrightness`, `sendImage`, and invalid `getTemperature` results return `success:false`.
- Semantic command failures do not isolate the subprocess, reconnect the controller, or automatically power off the panel.
- Invalid RJ1 temperature is non-finite, `<= -30.0 C`, or the legacy `-999.0` sentinel.
- The local UI uses orange `On - Command Error` and no modal dialog.
- Existing APL and over-temperature shutdown behavior remains unchanged.

---

## File Map

- `python/panel_worker.py`: validate driver results and expose stable semantic error markers.
- `tests/test_panel_worker_serial_binding.py`: executable worker result tests with fake hardware.
- `src/pythonbridge.cpp`: validate JSON-RPC data and preserve semantic error codes without worker isolation.
- `src/corneacontroller.cpp`: emit existing error signals for failed operations.
- `src/corneawidget.h`, `src/corneawidget.cpp`: avoid reconnecting on semantic failures and expose the controller's detailed error.
- `src/tcpserver.cpp`: expose stable TCP error codes and invalid-temperature failures.
- `src/devicecontrolpanel.h`, `src/devicecontrolpanel.cpp`: latch and display the powered-on command-error state.
- `tests/test_multipanel_worker_architecture.py`: source-level C++ propagation and UI regression checks.

---

### Task 1: Validate Python Worker Operation Results

**Files:**
- Modify: `tests/test_panel_worker_serial_binding.py`
- Modify: `python/panel_worker.py`

**Interfaces:**
- Produces: `BRIGHTNESS_SET_FAILED`, `IMAGE_TRANSFER_FAILED`, and structured temperature `ok` state.
- Consumed by: `PythonBridge::setBrightness`, `PythonBridge::writeFrame`, and `PythonBridge::getLeaTemperature`.

- [ ] **Step 1: Add fake operation hardware and failing worker tests**

Add a fake with configurable return values:

```python
class FakeOperationCornea:
    def __init__(self):
        self.set_result = None
        self.actual_brightness = 0.06
        self.frame_result = True
        self.temperature = 24.0

    def set_brightness(self, level):
        self.requested_brightness = level
        return self.set_result

    def get_brightness(self):
        return self.actual_brightness

    def write_rj1_frame(self, frame):
        return self.frame_result

    def get_lea_temperature(self):
        return self.temperature
```

Add tests to `PanelWorkerSerialBindingTests`:

```python
def test_set_brightness_rejects_literal_false(self):
    worker = panel_worker.Worker()
    worker.cornea = FakeOperationCornea()
    worker.cornea.set_result = False
    with self.assertRaisesRegex(RuntimeError, "BRIGHTNESS_SET_FAILED"):
        worker.cmd_setBrightness({"level": 0.06})

def test_set_brightness_rejects_readback_mismatch(self):
    worker = panel_worker.Worker()
    worker.cornea = FakeOperationCornea()
    worker.cornea.actual_brightness = 0.50
    with self.assertRaisesRegex(RuntimeError, "BRIGHTNESS_SET_FAILED"):
        worker.cmd_setBrightness({"level": 0.06})

def test_set_brightness_accepts_none_with_matching_readback(self):
    worker = panel_worker.Worker()
    worker.cornea = FakeOperationCornea()
    result = worker.cmd_setBrightness({"level": 0.06})
    self.assertAlmostEqual(0.06, result["level"])
    self.assertAlmostEqual(0.06, result["actual"])

def test_invalid_temperature_preserves_raw_value(self):
    worker = panel_worker.Worker()
    worker.cornea = FakeOperationCornea()
    worker.cornea.temperature = -40.0
    result = worker.cmd_getTemperature({})
    self.assertFalse(result["ok"])
    self.assertEqual(-40.0, result["temperature"])
```

For image failure, create a temporary `.npy` frame and assert the stable marker:

```python
def test_send_image_rejects_false_driver_result(self):
    import tempfile
    import numpy as np

    worker = panel_worker.Worker()
    worker.cornea = FakeOperationCornea()
    worker.cornea.frame_result = False
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "frame.npy"
        np.save(path, np.zeros((2, 2, 3), dtype=np.uint16))
        with self.assertRaisesRegex(RuntimeError, "IMAGE_TRANSFER_FAILED"):
            worker.cmd_sendImage({"path": str(path)})
```

- [ ] **Step 2: Run the worker tests and verify RED**

Run:

```powershell
python -m unittest tests.test_panel_worker_serial_binding.PanelWorkerSerialBindingTests -v
```

Expected: the new result-validation tests fail against the current permissive worker.

- [ ] **Step 3: Implement worker validation**

In `cmd_setBrightness`, preserve `None` compatibility while rejecting literal false and read-back mismatch:

```python
result = c.set_brightness(level)
if result is False:
    raise RuntimeError("BRIGHTNESS_SET_FAILED: set_brightness returned False")
actual = float(c.get_brightness())
if round(actual, 2) != round(level, 2):
    raise RuntimeError(
        f"BRIGHTNESS_SET_FAILED: requested={level:.2f}, actual={actual:.2f}"
    )
return {"level": level, "actual": actual}
```

In `cmd_sendImage`, reject a false frame result:

```python
ok_ = c.write_rj1_frame(frame)
if not bool(ok_):
    raise RuntimeError("IMAGE_TRANSFER_FAILED: write_rj1_frame returned False")
```

In `cmd_getTemperature`, preserve the raw value and mark its semantic validity:

```python
temp_value = float(temp) if temp is not None else -999.0
valid = math.isfinite(temp_value) and temp_value > -30.0
if valid:
    self.cached_temp = temp_value
return {
    "temperature": temp_value,
    "from_cache": False,
    "ok": valid,
}
```

Apply the same `ok` calculation to the `cached=True` branch. Add `import math`
and include both semantic markers in the main-loop stable error-code extraction
list.

- [ ] **Step 4: Run all worker tests and verify GREEN**

Run:

```powershell
python -m unittest tests.test_panel_worker_serial_binding -v
```

Expected: all worker tests pass, including the Windows named-mutex tests.

- [ ] **Step 5: Commit worker result validation**

```powershell
git add python/panel_worker.py tests/test_panel_worker_serial_binding.py
git commit -m "fix: validate worker command results"
```

---

### Task 2: Propagate Semantic Failures Through C++ and TCP

**Files:**
- Modify: `tests/test_multipanel_worker_architecture.py`
- Modify: `src/pythonbridge.cpp`
- Modify: `src/corneacontroller.cpp`
- Modify: `src/corneawidget.h`
- Modify: `src/corneawidget.cpp`
- Modify: `src/tcpserver.cpp`

**Interfaces:**
- Consumes: worker `errorCode`, `data.ok`, `data.actual`, and raw `data.temperature`.
- Produces: TCP errors `BRIGHTNESS_SET_FAILED`, `IMAGE_TRANSFER_FAILED`, and `INVALID_TEMPERATURE`.
- Preserves: `powerOn success:true` with an optional warning when its safe-brightness step fails.

- [ ] **Step 1: Add failing C++ architecture tests**

Add these assertions to `MultiPanelWorkerArchitectureTests`:

```python
def test_semantic_command_failures_propagate_without_worker_isolation(self):
    bridge = read("src/pythonbridge.cpp")
    widget = read("src/corneawidget.cpp")
    tcp = read("src/tcpserver.cpp")

    for code in (
        "BRIGHTNESS_SET_FAILED",
        "IMAGE_TRANSFER_FAILED",
        "INVALID_TEMPERATURE",
    ):
        self.assertIn(code, bridge + widget + tcp)
    self.assertIn('errorCode == QStringLiteral("IMAGE_TRANSFER_FAILED")', bridge)
    self.assertIn('firstError.contains(QStringLiteral("BRIGHTNESS_SET_FAILED"))', widget)

def test_tcp_temperature_rejects_minus_forty(self):
    body = function_body(
        read("src/tcpserver.cpp"),
        "QJsonObject TcpServer::handleGetTemperature",
    )
    self.assertIn("INVALID_TEMPERATURE", body)
    self.assertIn("-30.0", body)

def test_power_on_stays_successful_when_safe_brightness_warns(self):
    body = function_body(
        read("src/tcpserver.cpp"),
        "QJsonObject TcpServer::handlePowerOn",
    )
    self.assertIn("BRIGHTNESS_SET_FAILED", body)
    self.assertIn('response["warning"] = true', body)
    self.assertIn("return response", body)
```

- [ ] **Step 2: Run the architecture tests and verify RED**

Run:

```powershell
python -m unittest tests.test_multipanel_worker_architecture -v
```

Expected: new semantic error-code and power-on warning assertions fail.

- [ ] **Step 3: Validate bridge response data**

Update `PythonBridge::setBrightness` to preserve the worker error code/detail
and avoid `markSubprocessUnhealthy` for `BRIGHTNESS_SET_FAILED`.

Update `PythonBridge::writeFrame` so a successful envelope with
`data.ok=false` is converted to:

```cpp
setError(QStringLiteral(
    "IMAGE_TRANSFER_FAILED: write_rj1_frame returned False"));
return false;
```

When the envelope is already unsuccessful, skip worker isolation for
`IMAGE_TRANSFER_FAILED`; retain isolation for transport failures.

Update `PythonBridge::getLeaTemperature` to read `data.ok`, preserve the raw
`data.temperature` value, and call:

```cpp
setError(QString("INVALID_TEMPERATURE: invalid RJ1 temperature %1 C")
             .arg(temperature, 0, 'f', 1));
```

Return the raw temperature so UI and TCP can display the observed value.

- [ ] **Step 4: Emit controller errors and suppress semantic reconnects**

In `CorneaController::setBrightness` and `CorneaController::sendImage`, emit
`errorOccurred(m_bridge->getLastError())` when the bridge returns false.

In `CorneaWidget::setBrightnessBySerial`, treat
`BRIGHTNESS_SET_FAILED` like `USB_BUSY_TIMEOUT`: return false without
disconnect/reconnect. Preserve the full controller error in the detailed
brightness and image APIs instead of replacing it with generic text.

Add a read-only helper for the TCP layer:

```cpp
QString CorneaWidget::lastErrorForSerial(const QString &serial) const
{
    CorneaController *ctrl = getControllerBySerial(serial);
    return ctrl ? ctrl->lastError() : QString();
}
```

Declare `QString lastErrorForSerial(const QString &serial) const;` in
`corneawidget.h`.

- [ ] **Step 5: Return stable TCP failures and a power-on warning**

In `handleSetBrightness`, return `makeError()` with the detailed controller
error. In `handleGetTemperature`, reject non-finite values and values at or
below `-30.0`:

```cpp
if (!std::isfinite(temp) || temp <= -30.0) {
    return makeError(QString(
        "INVALID_TEMPERATURE: invalid RJ1 temperature %1 C")
        .arg(temp, 0, 'f', 1));
}
```

Extend `makeError()` with the three stable codes.

In `handlePowerOn`, keep the current successful power result. Check the safe
brightness call and, when it fails, return:

```cpp
QJsonObject response = makeSuccess();
response["warning"] = true;
response["warningCode"] = QStringLiteral("BRIGHTNESS_SET_FAILED");
response["warningMessage"] = m_corneaWidget->lastErrorForSerial(serial);
return response;
```

- [ ] **Step 6: Run architecture tests and verify GREEN**

Run the command from Step 2.

Expected: all architecture tests pass.

- [ ] **Step 7: Commit C++ and TCP propagation**

```powershell
git add tests/test_multipanel_worker_architecture.py src/pythonbridge.cpp src/corneacontroller.cpp src/corneawidget.h src/corneawidget.cpp src/tcpserver.cpp
git commit -m "fix: propagate hardware command failures"
```

---

### Task 3: Display Powered-On Command Errors Locally

**Files:**
- Modify: `tests/test_multipanel_worker_architecture.py`
- Modify: `src/devicecontrolpanel.h`
- Modify: `src/devicecontrolpanel.cpp`

**Interfaces:**
- Consumes: `CorneaController::errorOccurred` and raw RJ1 temperature values.
- Produces: latched `m_commandError`, orange `On - Command Error`, and red invalid-temperature text.

- [ ] **Step 1: Add failing UI-state architecture test**

```python
def test_powered_panel_latches_command_error_ui(self):
    header = read("src/devicecontrolpanel.h")
    source = read("src/devicecontrolpanel.cpp")

    self.assertIn("bool m_commandError = false;", header)
    self.assertIn('m_lblStatus->setText("On - Command Error")', source)
    self.assertIn("color: darkorange", source)
    self.assertIn('QString("Invalid (%1 C)")', source)
    self.assertIn("m_commandError = true", source)
```

- [ ] **Step 2: Run the focused UI test and verify RED**

Run:

```powershell
python -m unittest tests.test_multipanel_worker_architecture.MultiPanelWorkerArchitectureTests.test_powered_panel_latches_command_error_ui -v
```

Expected: FAIL because the command-error latch and display do not exist.

- [ ] **Step 3: Implement the latched status state**

Add `bool m_commandError = false;` to `DeviceControlPanel`.

In `onControllerError`, queue-safe latch the state when powered on, write the
error to the panel log, and call `updateUIState()`.

In `updateUIState`, place the command-error branch before the normal powered
branch:

```cpp
} else if (poweredOn && m_commandError) {
    m_lblStatus->setText("On - Command Error");
    m_lblStatus->setStyleSheet(
        "color: darkorange; font-weight: bold;");
} else if (poweredOn) {
```

Clear `m_commandError` on power-off/disconnect and at the start of a new power
cycle. Do not clear it merely because a later unrelated command succeeds.

- [ ] **Step 4: Display invalid RJ1 temperature in red**

In `onControllerTemperatureUpdated`, handle RJ1 separately from the optional
DA9272 probe:

```cpp
const bool invalidRj1 = !std::isfinite(rj1)
                     || (rj1 <= -30.0 && rj1 > -900.0);
if (invalidRj1) {
    m_lblRj1Temp->setText(QString("Invalid (%1 C)").arg(rj1, 0, 'f', 1));
    m_lblRj1Temp->setStyleSheet("color: red; font-weight: bold;");
    m_commandError = true;
    updateUIState();
} else {
    m_lblRj1Temp->setStyleSheet(QString());
    m_lblRj1Temp->setText(rj1 <= -900.0
        ? QStringLiteral("-")
        : QString("%1 C").arg(rj1, 0, 'f', 1));
}
```

Keep `-999.0` as the existing unavailable display `-`; the command-facing TCP
path still rejects it as invalid.

- [ ] **Step 5: Run architecture tests and verify GREEN**

Run:

```powershell
python -m unittest tests.test_multipanel_worker_architecture -v
```

Expected: all architecture tests pass.

- [ ] **Step 6: Commit local UI feedback**

```powershell
git add tests/test_multipanel_worker_architecture.py src/devicecontrolpanel.h src/devicecontrolpanel.cpp
git commit -m "feat: show powered-on command errors"
```

---

### Task 4: Full Regression and Release Verification

**Files:**
- Verify all modified files.

**Interfaces:**
- Verifies the complete worker-to-TCP/UI result path for version `1.1.26`.

- [ ] **Step 1: Run the full Python test suite**

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

Expected: zero failures and zero errors.

- [ ] **Step 2: Compile the isolated worktree Release build**

Use Visual Studio 2022 Build Tools and Qt 5.15.2 to run qmake and nmake inside
`build/codex-release`; do not use `build_release.bat` because it hardcodes the
main worktree build directory.

Expected artifact:

```text
build/codex-release/release/CorneaController.exe
```

Expected: qmake/nmake exit code 0. Existing C4819, LNK4099, and empty-resource
warnings are non-fatal.

- [ ] **Step 3: Inspect tracked scope and version**

```powershell
git status --short
git diff --check HEAD~3 HEAD
Select-String -Path src\main.cpp -Pattern 'setApplicationVersion("1.1.26")'
```

Expected: clean tracked worktree, no whitespace errors, and version `1.1.26`.

- [ ] **Step 4: Record station acceptance sequence**

Run against one known-good panel and one panel reproducing the field issue:

```text
powerOn        -> success:true
setBrightness -> success:false when write/read-back fails
sendImage     -> success:false when write_rj1_frame returns false
getTemperature-> success:false for -40.0 C
```

Confirm the panel stays powered, the local status is orange after a failed
command, and power-off clears the command-error state.
