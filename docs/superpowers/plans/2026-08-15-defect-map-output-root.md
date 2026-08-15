# Independent Defect-Map Output Root Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep HDF5 calibration data on the local `cal_path` while allowing automatic defect-map PNG exports to use an optional independent local or UNC root.

**Architecture:** Extend `PythonConfig` with an optional `defectMapOutputRoot`, load/save it under `python.defect_map_output_root`, and set it on `PythonBridge` during widget initialization. `PythonBridge` forwards the value in each subprocess `init` request; the worker's existing path selector consumes it and retains its existing empty-value fallback.

**Tech Stack:** C++17, Qt 5.15 JSON/configuration APIs, Python 3.12 `unittest`, line-delimited JSON subprocess RPC.

## Global Constraints

- `cal_path` remains the only source for HDF5 calibration lookup.
- A non-empty output root produces `<defect_map_output_root>/<Panel ID>/`.
- An absent or empty output root preserves `<cal_path>/defect_maps/<Panel ID>/`.
- Export failure remains non-fatal to otherwise successful initialization and power-on.
- No SMB credentials, share mounting, or authentication retries are added.
- The embedded-Python debug fallback remains outside scope because it does not implement the existing automatic export.

---

### Task 1: Lock the Existing Worker Path Contract

**Files:**
- Modify: `python/test_panel_worker_defect_maps.py`
- Verify without modifying: `python/panel_worker.py:249-259,399-447`

**Interfaces:**
- Consumes: `Worker.defect_map_output_root: str` and `Worker._defect_map_output_dir(panel_id: str) -> Path`.
- Produces: Regression coverage proving HDF5 input and PNG output can use different roots.

- [ ] **Step 1: Add a characterization test for separate input/output roots**

Add this test to `DefectMapExportTests`:

```python
def test_configured_output_root_keeps_hdf5_input_local(self):
    with tempfile.TemporaryDirectory() as tmp:
        worker = Worker()
        worker.cornea = FakeCornea()
        worker.cal_path = str(Path(tmp) / "local_hdf5")
        worker.defect_map_output_root = str(Path(tmp) / "remote_share")

        def fake_run(cmd, **kwargs):
            class Result:
                returncode = 0
                stdout = "ok"
                stderr = ""
            return Result()

        with patch("panel_worker.subprocess.run", side_effect=fake_run) as run:
            result = worker.cmd_powerOn({})

        cmd = run.call_args.args[0]
        self.assertEqual(cmd[3], worker.cal_path)
        self.assertEqual(
            Path(cmd[6]), Path(worker.defect_map_output_root) / "04830377"
        )
        self.assertEqual(Path(result["defect_maps"]["output_path"]), Path(cmd[6]))
```

- [ ] **Step 2: Run the characterization test**

Run:

```powershell
C:\Python312\python.exe -m unittest python/test_panel_worker_defect_maps.py -v
```

Expected: PASS. This is existing worker behavior; the new missing behavior is the C++ configuration/wiring covered by Task 2's RED test.

- [ ] **Step 3: Commit the regression coverage**

```bash
git add python/test_panel_worker_defect_maps.py
git commit -m "test: cover independent defect map output root"
```

### Task 2: Wire the Setting from Configuration to Every Worker

**Files:**
- Modify: `tests/test_multipanel_worker_architecture.py`
- Modify: `src/corneaconfig.h:8-21`
- Modify: `src/corneaconfig.cpp:72-89,127-146`
- Modify: `src/pythonbridge.h:35-65,150-170`
- Modify: `src/pythonbridge.cpp:1438-1462`
- Modify: `src/corneawidget.cpp:228-265`
- Modify: `cornea_config.example.json`

**Interfaces:**
- Consumes: JSON key `python.defect_map_output_root` as a string; missing values become an empty `QString`.
- Produces: `PythonConfig::defectMapOutputRoot`, `PythonBridge::setDefectMapOutputRoot(const QString &)`, and worker init argument `defect_map_output_root`.

- [ ] **Step 1: Write the failing end-to-end wiring test**

Add this test to `MultiPanelWorkerArchitectureTests`:

```python
def test_defect_map_output_root_is_configured_end_to_end(self):
    config_h = read("src/corneaconfig.h")
    config_cpp = read("src/corneaconfig.cpp")
    bridge_h = read("src/pythonbridge.h")
    bridge_cpp = read("src/pythonbridge.cpp")
    widget_cpp = read("src/corneawidget.cpp")
    example = read("cornea_config.example.json")

    self.assertIn("QString defectMapOutputRoot;", config_h)
    self.assertIn('["defect_map_output_root"].toString()', config_cpp)
    self.assertIn(
        '["defect_map_output_root"] = m_python.defectMapOutputRoot', config_cpp
    )
    self.assertIn("setDefectMapOutputRoot", bridge_h)
    self.assertIn("m_defectMapOutputRoot", bridge_h)
    self.assertIn('{"defect_map_output_root", m_defectMapOutputRoot}', bridge_cpp)
    self.assertIn("setDefectMapOutputRoot(py.defectMapOutputRoot)", widget_cpp)
    self.assertIn('"defect_map_output_root": ""', example)
```

The production mutation this catches is any dropped link between JSON loading and the worker `init` payload.

- [ ] **Step 2: Run the test and verify RED**

Run:

```powershell
C:\Python312\python.exe -m unittest tests.test_multipanel_worker_architecture.MultiPanelWorkerArchitectureTests.test_defect_map_output_root_is_configured_end_to_end -v
```

Expected: FAIL because `PythonConfig` does not yet contain `defectMapOutputRoot`.

- [ ] **Step 3: Implement minimal configuration persistence**

Add to `PythonConfig` in `src/corneaconfig.h`:

```cpp
QString defectMapOutputRoot;
```

Load and save it in `src/corneaconfig.cpp`:

```cpp
m_python.defectMapOutputRoot = pythonObj["defect_map_output_root"].toString();
```

```cpp
pythonObj["defect_map_output_root"] = m_python.defectMapOutputRoot;
```

- [ ] **Step 4: Implement minimal bridge propagation**

Add to `PythonBridge`'s public configuration methods and private state:

```cpp
void setDefectMapOutputRoot(const QString &path) { m_defectMapOutputRoot = path; }
```

```cpp
QString m_defectMapOutputRoot;
```

Set it in `CorneaWidget::initializePythonBridge()` before the bridge is initialized:

```cpp
m_pythonBridge->setDefectMapOutputRoot(py.defectMapOutputRoot);
```

Forward it in the `initArgs` object in `PythonBridge::createDeviceInstance()`:

```cpp
{"defect_map_output_root", m_defectMapOutputRoot},
```

- [ ] **Step 5: Add the backward-compatible example field**

Add beside `cal_path` in `cornea_config.example.json`:

```json
"defect_map_output_root": ""
```

- [ ] **Step 6: Run targeted tests and verify GREEN**

Run:

```powershell
C:\Python312\python.exe -m unittest tests.test_multipanel_worker_architecture python/test_panel_worker_defect_maps.py -v
```

Expected: all tests PASS.

- [ ] **Step 7: Build the Release application**

Run:

```powershell
build_release.bat
```

Expected: `nmake` exits with code 0 and produces the Release executable.

- [ ] **Step 8: Commit the feature**

```bash
git add src/corneaconfig.h src/corneaconfig.cpp src/pythonbridge.h src/pythonbridge.cpp src/corneawidget.cpp cornea_config.example.json tests/test_multipanel_worker_architecture.py
git commit -m "feat: configure independent defect map output root"
```

### Task 3: Document UNC Operation and Run Final Verification

**Files:**
- Modify: `docs/INTEGRATION_GUIDE.md:195-245`
- Modify: `README.md:82-120`

**Interfaces:**
- Consumes: `python.defect_map_output_root` from Task 2.
- Produces: Operator-facing configuration example, permission requirements, fallback behavior, and resulting output path.

- [ ] **Step 1: Update the configuration examples**

Use a local HDF5 path and an independently escaped UNC output path:

```json
"cal_path": "C:/google_env/hdf5_files",
"defect_map_output_root": "\\\\REMOTE-PC\\DefectMaps"
```

- [ ] **Step 2: Document path behavior and prerequisites**

State all of the following in both the concise README configuration section and the detailed integration guide:

```text
The running Windows account must have create/write permission on the share.
Configured output: \\REMOTE-PC\DefectMaps\<Panel ID>\
Empty/missing fallback: <cal_path>\defect_maps\<Panel ID>\
HDF5 files remain under cal_path and are not copied to the output share.
An unreachable/unwritable share logs an export warning but does not fail Power On.
```

- [ ] **Step 3: Run the complete Python regression suite**

Run:

```powershell
C:\Python312\python.exe -m unittest discover -s tests -p "test_*.py" -v
C:\Python312\python.exe -m unittest discover -s python -p "test_*.py" -v
```

Expected: all tests PASS.

- [ ] **Step 4: Rebuild Release after documentation/config changes**

Run:

```powershell
build_release.bat
```

Expected: `nmake` exits with code 0.

- [ ] **Step 5: Review the final diff**

Run:

```bash
git diff --check
git status --short
git diff --stat HEAD
```

Expected: no whitespace errors; only the planned implementation, tests, and documentation remain in scope, with unrelated pre-existing untracked files untouched.

- [ ] **Step 6: Commit the documentation**

```bash
git add README.md docs/INTEGRATION_GUIDE.md
git commit -m "docs: explain remote defect map export"
```
