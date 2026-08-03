# Configurable USB Serialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cross-device FTDI/USB serialization configurable and disabled by default while preserving the current serialized mode as a station-level fallback.

**Architecture:** Add one `python.serialize_usb` configuration value and propagate it through `CorneaWidget` to `TcpServer`, `PythonBridge`, and each `panel_worker.py` subprocess. Every process-wide gate conditionally bypasses locking when the value is false; each panel's dedicated `PanelSubprocess` thread continues to serialize commands for that panel.

**Tech Stack:** C++17, Qt 5.15 (`QReadWriteLock`, `QProcess`, JSON configuration), Python 3.12 `unittest`, Windows named mutex.

## Global Constraints

- The compiled default and missing-key behavior must be `serialize_usb=false`.
- `serialize_usb=true` must retain the existing global serialization behavior.
- Per-panel command ordering, serial validation, subprocess isolation, TCP response semantics, and USB error codes must remain unchanged.
- The mode is fixed for one application process lifetime; no per-command TCP override is added.
- Existing unrelated untracked files in the worktree must not be staged or modified.

---

## File Map

- `src/corneaconfig.h`, `src/corneaconfig.cpp`: own, parse, and save the station setting.
- `src/corneawidget.cpp`: propagate the setting during startup and log the active mode.
- `src/tcpserver.h`, `src/tcpserver.cpp`: conditionally bypass the process-wide TCP read/write lock.
- `src/pythonbridge.h`, `src/pythonbridge.cpp`: conditionally bypass C++ embedded/subprocess USB gates and set the child environment.
- `python/panel_worker.py`: conditionally bypass the named Windows USB mutex.
- `cornea_config.example.json`: document the default configuration.
- `tests/test_multipanel_worker_architecture.py`: source-level configuration and C++ wiring regression tests.
- `tests/test_panel_worker_serial_binding.py`: executable Python gate-selection tests.

---

### Task 1: Configuration Contract

**Files:**
- Modify: `tests/test_multipanel_worker_architecture.py`
- Modify: `src/corneaconfig.h`
- Modify: `src/corneaconfig.cpp`
- Modify: `cornea_config.example.json`

**Interfaces:**
- Produces: `PythonConfig::serializeUsb` (`bool`, default `false`).
- Consumed later by: `CorneaWidget`, `TcpServer`, and `PythonBridge`.

- [ ] **Step 1: Write the failing configuration test**

Add this test to `MultiPanelWorkerArchitectureTests`:

```python
def test_usb_serialization_config_defaults_off_and_is_persisted(self):
    header = read("src/corneaconfig.h")
    source = read("src/corneaconfig.cpp")
    example = read("cornea_config.example.json")

    self.assertIn("bool serializeUsb = false;", header)
    self.assertIn('["serialize_usb"].toBool(false)', source)
    self.assertIn('["serialize_usb"] = m_python.serializeUsb', source)
    self.assertIn('"serialize_usb": false', example)
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
python -m unittest tests.test_multipanel_worker_architecture.MultiPanelWorkerArchitectureTests.test_usb_serialization_config_defaults_off_and_is_persisted -v
```

Expected: FAIL because `serializeUsb` and `serialize_usb` do not exist.

- [ ] **Step 3: Implement the minimal configuration field**

Add to `PythonConfig`:

```cpp
bool serializeUsb = false;
```

Load with the requested missing-key default:

```cpp
m_python.serializeUsb = pythonObj["serialize_usb"].toBool(false);
```

Save explicitly:

```cpp
pythonObj["serialize_usb"] = m_python.serializeUsb;
```

Add the example key under `python`:

```json
"serialize_usb": false
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the command from Step 2.

Expected: PASS.

- [ ] **Step 5: Commit the configuration contract**

```powershell
git add tests/test_multipanel_worker_architecture.py src/corneaconfig.h src/corneaconfig.cpp cornea_config.example.json
git commit -m "feat: add USB serialization config"
```

---

### Task 2: Python Worker Gate Selection

**Files:**
- Modify: `tests/test_panel_worker_serial_binding.py`
- Modify: `python/panel_worker.py`

**Interfaces:**
- Consumes: `CORNEA_SERIALIZE_USB=0|1` from the child process environment.
- Produces: `_env_flag(name: str, default: bool = False) -> bool`, `USB_SERIALIZATION_ENABLED`, and `_command_requires_usb_gate(command: str) -> bool`.

- [ ] **Step 1: Write failing environment and command-selection tests**

Add to `PanelWorkerSerialBindingTests`:

```python
def test_usb_serialization_defaults_disabled(self):
    with patch.dict(os.environ, {}, clear=True):
        self.assertFalse(panel_worker._env_flag("CORNEA_SERIALIZE_USB", False))

def test_usb_serialization_accepts_explicit_true(self):
    with patch.dict(os.environ, {"CORNEA_SERIALIZE_USB": "1"}, clear=True):
        self.assertTrue(panel_worker._env_flag("CORNEA_SERIALIZE_USB", False))

def test_ping_never_uses_usb_gate(self):
    with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", True):
        self.assertFalse(panel_worker._command_requires_usb_gate("ping"))

def test_hardware_command_uses_gate_only_when_enabled(self):
    with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", False):
        self.assertFalse(panel_worker._command_requires_usb_gate("init"))
    with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", True):
        self.assertTrue(panel_worker._command_requires_usb_gate("init"))
```

- [ ] **Step 2: Run the worker tests and verify RED**

Run:

```powershell
python -m unittest tests.test_panel_worker_serial_binding.PanelWorkerSerialBindingTests -v
```

Expected: FAIL because `_env_flag` and `_command_requires_usb_gate` do not exist.

- [ ] **Step 3: Implement environment parsing and conditional gate use**

Add near the logger setup:

```python
def _env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


USB_SERIALIZATION_ENABLED = _env_flag("CORNEA_SERIALIZE_USB", False)


def _command_requires_usb_gate(command: str) -> bool:
    return USB_SERIALIZATION_ENABLED and command != "ping"
```

Replace the unconditional worker decision with:

```python
gate_required = _command_requires_usb_gate(cmd)
```

Log once from `main()`:

```python
log.info(
    "USB serialization %s",
    "ENABLED" if USB_SERIALIZATION_ENABLED else "DISABLED",
)
```

- [ ] **Step 4: Run all worker tests and verify GREEN**

Run:

```powershell
python -m unittest tests.test_panel_worker_serial_binding -v
```

Expected: all tests PASS, including the existing Windows named-mutex tests.

- [ ] **Step 5: Commit the worker behavior**

```powershell
git add tests/test_panel_worker_serial_binding.py python/panel_worker.py
git commit -m "feat: make worker USB gate configurable"
```

---

### Task 3: C++ Gate Propagation and Conditional Locking

**Files:**
- Modify: `tests/test_multipanel_worker_architecture.py`
- Modify: `src/pythonbridge.h`
- Modify: `src/pythonbridge.cpp`
- Modify: `src/tcpserver.h`
- Modify: `src/tcpserver.cpp`
- Modify: `src/corneawidget.cpp`

**Interfaces:**
- Consumes: `PythonConfig::serializeUsb`.
- Produces: `PythonBridge::setSerializeUsb(bool)`, `PythonBridge::serializeUsb() const`, and `TcpServer::setSerializeUsb(bool)`.
- Child contract: `CORNEA_SERIALIZE_USB=0|1` is set before `PanelSubprocess::spawn()`.

- [ ] **Step 1: Write failing C++ architecture tests**

Replace the existing always-global-gate assertion and add wiring assertions:

```python
def test_usb_serialization_setting_controls_all_cpp_gates(self):
    bridge_h = read("src/pythonbridge.h")
    bridge_cpp = read("src/pythonbridge.cpp")
    tcp_h = read("src/tcpserver.h")
    tcp_cpp = read("src/tcpserver.cpp")
    widget_cpp = read("src/corneawidget.cpp")

    self.assertIn("setSerializeUsb(bool on)", bridge_h)
    self.assertIn("bool m_serializeUsb = false;", bridge_h)
    self.assertIn("setSerializeUsb(bool on)", tcp_h)
    self.assertIn("bool m_serializeUsb = false;", tcp_h)
    self.assertIn("m_serializeUsb", tcp_cpp)
    self.assertIn("CORNEA_SERIALIZE_USB=", bridge_cpp)
    self.assertIn("m_serializeUsb", bridge_cpp)
    self.assertIn("setSerializeUsb(py.serializeUsb)", widget_cpp)

def test_subprocess_usb_gate_remains_available_when_enabled(self):
    bridge_cpp = read("src/pythonbridge.cpp")
    worker_py = read("python/panel_worker.py")
    self.assertIn("LibusbGateLease", bridge_cpp)
    self.assertIn("USB_BUSY_TIMEOUT", bridge_cpp)
    self.assertIn("CorneaController_USB_GATE_v1", worker_py)
```

- [ ] **Step 2: Run the architecture tests and verify RED**

Run:

```powershell
python -m unittest tests.test_multipanel_worker_architecture -v
```

Expected: FAIL because the setters, fields, environment propagation, and conditional branches do not exist.

- [ ] **Step 3: Add the bridge and TCP mode APIs**

In `PythonBridge`:

```cpp
void setSerializeUsb(bool on) { m_serializeUsb = on; }
bool serializeUsb() const { return m_serializeUsb; }
// private
bool m_serializeUsb = false;
```

In `TcpServer`:

```cpp
void setSerializeUsb(bool on) { m_serializeUsb = on; }
// private
bool m_serializeUsb = false;
```

- [ ] **Step 4: Make `LibusbGateLease` support an explicit bypass**

Change its constructor to accept `bool enabled`. When disabled, set
`m_acquired=true`, `m_bypassed=true`, and return without touching the mutex.
The destructor must return immediately when `m_bypassed` is true.

```cpp
LibusbGateLease(bool enabled, const QString &command,
                const QString &serial, int waitTimeoutMs)
{
    if (!enabled) {
        m_acquired = true;
        m_bypassed = true;
        return;
    }
    // existing acquisition path
}
```

Pass `m_serializeUsb` at every lease site: init, teardown, isolate, and normal
subprocess calls. This preserves existing `acquired()` call-site behavior.

- [ ] **Step 5: Make embedded dispatch locking conditional**

Add a `serializeUsb` parameter to `invokeOnWorker`, capture it in the queued
lambda, and allocate the locker only when enabled:

```cpp
std::unique_ptr<QMutexLocker> libusbLock;
qint64 lockWait = 0;
if (serializeUsb) {
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    libusbLock = std::make_unique<QMutexLocker>(&g_libusbGlobalMutex);
    lockWait = QDateTime::currentMSecsSinceEpoch() - started;
}
```

Pass `m_serializeUsb` from `dispatch()` and `dispatchToDevice()`.

- [ ] **Step 6: Propagate the worker environment before spawn**

Before `proc->spawn(10000)`:

```cpp
proc->setExtraEnv(QStringList{
    QString("CORNEA_SERIALIZE_USB=%1").arg(m_serializeUsb ? 1 : 0)
});
```

Reused idle subprocesses are safe because the mode is fixed for the application
lifetime.

- [ ] **Step 7: Make the TCP read/write lock conditional**

Only acquire and release `m_deviceLock` when `m_serializeUsb` is true. Preserve
the existing exclusive/shared distinction inside that branch:

```cpp
bool lockHeld = false;
if (m_serializeUsb) {
    if (isExclusive) m_deviceLock.lockForWrite();
    else m_deviceLock.lockForRead();
    lockHeld = true;
}
// process command
if (lockHeld) m_deviceLock.unlock();
```

- [ ] **Step 8: Wire startup and mode logging**

In `CorneaWidget::initializePythonBridge()`:

```cpp
m_pythonBridge->setSerializeUsb(py.serializeUsb);
appendLog(QString("USB serialization: %1")
              .arg(py.serializeUsb ? "ENABLED" : "DISABLED"));
```

In `CorneaWidget::startTcpServer()` set the same value before calling `start()`:

```cpp
m_tcpServer->setSerializeUsb(m_config.python().serializeUsb);
```

- [ ] **Step 9: Run architecture tests and verify GREEN**

Run the command from Step 2.

Expected: all tests PASS.

- [ ] **Step 10: Commit C++ conditional locking**

```powershell
git add tests/test_multipanel_worker_architecture.py src/pythonbridge.h src/pythonbridge.cpp src/tcpserver.h src/tcpserver.cpp src/corneawidget.cpp
git commit -m "feat: default USB operations to concurrent mode"
```

---

### Task 4: Full Regression and Build Verification

**Files:**
- Verify only; modify production files only if a test or compiler error identifies a defect in Tasks 1-3.

**Interfaces:**
- Verifies the complete `serialize_usb` configuration path and preserves existing APIs.

- [ ] **Step 1: Run the full Python test suite**

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

Expected: all tests PASS with zero failures and zero errors.

- [ ] **Step 2: Compile the release application**

```powershell
cmd /c build_release.bat
```

Expected: `nmake` exits with code 0 and produces the release executable.

- [ ] **Step 3: Inspect the final tracked diff**

```powershell
git status --short
git diff HEAD~3 -- src python tests cornea_config.example.json docs/superpowers
```

Confirm that unrelated pre-existing untracked files are absent from commits and
that every global USB lock checks `serializeUsb` or
`USB_SERIALIZATION_ENABLED`.

- [ ] **Step 4: Run a no-hardware configuration smoke check**

```powershell
Select-String -Path cornea_config.example.json -Pattern '"serialize_usb": false'
Select-String -Path src\corneawidget.cpp -Pattern 'USB serialization:'
Select-String -Path python\panel_worker.py -Pattern 'USB serialization'
```

Expected: all three commands find the requested default and diagnostics.

- [ ] **Step 5: Record hardware validation commands for the station**

Use the existing concurrent benchmark with two known-good serials:

```powershell
python scripts\bench_n_panel_scan.py --help
```

Select the station's two connected serials and execute 100 concurrent cold
cycles. Acceptance criteria are zero process hangs, zero serial mismatches,
zero `USB_TRANSPORT_LOST`, no unexplained NACK increase, and two-panel wall
clock close to one-panel initialization time. Re-run with
`"serialize_usb": true` to confirm the fallback remains serialized.

- [ ] **Step 6: Commit any verification-only correction**

Skip this step when no files changed. If a compiler/test correction was needed,
stage only those files and commit with a message describing that exact defect.
