from __future__ import annotations

import sys
import types
import unittest
import os
import subprocess
import threading
import time
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

import panel_worker


class FakeCornea:
    enumerations = [([5, 2], ["OTHER", "EXPECTED"])] * 2
    actual_serial = "EXPECTED"
    constructed_index = None
    powered_off = False

    @classmethod
    def get_available_corneas(cls):
        return cls.enumerations.pop(0)

    def __init__(self, **kwargs):
        type(self).constructed_index = kwargs["cornea_index"]
        self.cornea_serial = type(self).actual_serial
        self.init_ok = True
        self.state_vals = {"unique_chip_id_str": "PANEL"}

    def system_power_off(self):
        type(self).powered_off = True


class FakeUsbTools:
    @staticmethod
    def flush_cache():
        return None


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


def fake_modules():
    ar_root = types.ModuleType("ar_display_lab_lib")
    control_boards = types.ModuleType("ar_display_lab_lib.control_boards")
    cornea_mod = types.ModuleType(
        "ar_display_lab_lib.control_boards.cornea_rax720"
    )
    cornea_mod.CorneaRax720 = FakeCornea
    pyftdi_root = types.ModuleType("pyftdi")
    usbtools_mod = types.ModuleType("pyftdi.usbtools")
    usbtools_mod.UsbTools = FakeUsbTools
    return {
        "ar_display_lab_lib": ar_root,
        "ar_display_lab_lib.control_boards": control_boards,
        "ar_display_lab_lib.control_boards.cornea_rax720": cornea_mod,
        "pyftdi": pyftdi_root,
        "pyftdi.usbtools": usbtools_mod,
    }


class PanelWorkerSerialBindingTests(unittest.TestCase):
    def setUp(self):
        FakeCornea.enumerations = [
            ([5, 2], ["OTHER", "EXPECTED"]),
            ([5, 2], ["OTHER", "EXPECTED"]),
        ]
        FakeCornea.actual_serial = "EXPECTED"
        FakeCornea.constructed_index = None
        FakeCornea.powered_off = False

    def init_args(self):
        return {
            "cornea_index": 5,
            "cornea_serial": "EXPECTED",
            "hardware_variant": "F33L",
            "cal_path": "",
            "init_cornea": True,
            "init_rj1": True,
        }

    def test_usb_serialization_defaults_disabled(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertFalse(
                panel_worker._env_flag("CORNEA_SERIALIZE_USB", False)
            )

    def test_usb_serialization_accepts_explicit_true(self):
        with patch.dict(
            os.environ, {"CORNEA_SERIALIZE_USB": "1"}, clear=True
        ):
            self.assertTrue(
                panel_worker._env_flag("CORNEA_SERIALIZE_USB", False)
            )

    def test_ping_never_uses_usb_gate(self):
        with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", True):
            self.assertFalse(panel_worker._command_requires_usb_gate("ping"))

    def test_hardware_command_uses_gate_only_when_enabled(self):
        with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", False):
            self.assertFalse(panel_worker._command_requires_usb_gate("init"))
        with patch.object(panel_worker, "USB_SERIALIZATION_ENABLED", True):
            self.assertTrue(panel_worker._command_requires_usb_gate("init"))

    def test_resolves_current_index_by_stable_expected_serial(self):
        worker = panel_worker.Worker()
        with patch.dict(sys.modules, fake_modules()), patch.object(
            panel_worker.time, "sleep", return_value=None
        ), patch.object(
            worker, "_require_panel_id", return_value="PANEL"
        ), patch.object(
            worker, "_export_defect_maps_once", return_value={"ok": True}
        ):
            result = worker.cmd_init(self.init_args())

        self.assertTrue(result["init_ok"])
        self.assertEqual("EXPECTED", result["cornea_serial"])
        self.assertEqual(2, FakeCornea.constructed_index)

    def test_missing_serial_is_rejected_before_construction(self):
        FakeCornea.enumerations = [
            ([5], ["OTHER"]),
            ([5], ["OTHER"]),
        ]
        worker = panel_worker.Worker()
        with patch.dict(sys.modules, fake_modules()), patch.object(
            panel_worker.time, "sleep", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, "SERIAL_NOT_FOUND"):
                worker.cmd_init(self.init_args())
        self.assertIsNone(FakeCornea.constructed_index)

    def test_actual_serial_mismatch_is_powered_off_and_rejected(self):
        FakeCornea.actual_serial = "WRONG"
        worker = panel_worker.Worker()
        with patch.dict(sys.modules, fake_modules()), patch.object(
            panel_worker.time, "sleep", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, "SERIAL_MISMATCH"):
                worker.cmd_init(self.init_args())
        self.assertTrue(FakeCornea.powered_off)
        self.assertIsNone(worker.cornea)

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


@unittest.skipUnless(os.name == "nt", "Windows named mutex behavior")
class WindowsNamedUsbGateTests(unittest.TestCase):
    def test_named_gate_serializes_distinct_handles(self):
        first = panel_worker._UsbGate()
        second = panel_worker._UsbGate()
        first.acquire(1.0)
        acquired_after = []

        def waiter():
            started = time.monotonic()
            second.acquire(2.0)
            acquired_after.append(time.monotonic() - started)
            second.release()

        thread = threading.Thread(target=waiter)
        thread.start()
        time.sleep(0.2)
        first.release()
        thread.join(2.0)
        first.close()
        second.close()

        self.assertFalse(thread.is_alive())
        self.assertGreaterEqual(acquired_after[0], 0.15)

    def test_named_gate_is_released_after_worker_crash(self):
        code = (
            "import sys,time;"
            f"sys.path.insert(0,{str(ROOT / 'python')!r});"
            "from panel_worker import _UsbGate;"
            "g=_UsbGate();g.acquire(2);"
            "print('ready',flush=True);time.sleep(60)"
        )
        child = subprocess.Popen(
            [sys.executable, "-c", code],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertEqual("ready", child.stdout.readline().strip())
            child.kill()
            child.wait(timeout=5)

            gate = panel_worker._UsbGate()
            started = time.monotonic()
            gate.acquire(2.0)
            elapsed = time.monotonic() - started
            gate.release()
            gate.close()
            self.assertLess(elapsed, 1.0)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            if child.stdout:
                child.stdout.close()
            if child.stderr:
                child.stderr.close()


if __name__ == "__main__":
    unittest.main()
