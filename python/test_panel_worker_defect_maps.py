from __future__ import annotations

import tempfile
import unittest
import logging
from pathlib import Path
from unittest.mock import patch

from panel_worker import Worker


class FakeCornea:
    def __init__(self, panel_id: str = "04830377"):
        self.panel_id = panel_id

    def system_power_on(self):
        return True

    def get_rj1_chip_info_decoded(self):
        return {"unique_chip_id_str": self.panel_id}


class FakeCorneaPowerOffFailure:
    def __init__(self):
        self.logger = logging.getLogger(f"{__name__}.FakeCorneaPowerOffFailure")

    def system_power_off(self):
        self.logger.error("Failed to complete cornea_power_down. e=I2cNackError('NACK from slave')")


class FakeLoggerAdapter:
    def __init__(self, logger):
        self.logger = logger

    def error(self, message):
        self.logger.error(message)


class FakeCorneaPowerOffFailureWithLoggerAdapter(FakeCorneaPowerOffFailure):
    def __init__(self):
        self.logger = FakeLoggerAdapter(logging.getLogger(f"{__name__}.FakeCorneaPowerOffFailureWithLoggerAdapter"))


class FakeCorneaPowerOnFailure(FakeCornea):
    def __init__(self):
        super().__init__(panel_id="04830377")
        self.logger = logging.getLogger(f"{__name__}.FakeCorneaPowerOnFailure")

    def system_power_on(self):
        self.logger.error("Exception: NACK from slave. Failure to sequence power supplies.")
        return False


class DefectMapExportTests(unittest.TestCase):
    def test_power_on_exports_defect_maps_once_after_panel_id_is_available(self):
        with tempfile.TemporaryDirectory() as tmp:
            worker = Worker()
            worker.cornea = FakeCornea()
            worker.cal_path = str(Path(tmp) / "hdf5_files")

            calls = []

            def fake_run(cmd, **kwargs):
                calls.append((cmd, kwargs))

                class Result:
                    returncode = 0
                    stdout = "ok"
                    stderr = ""

                return Result()

            with patch("panel_worker.subprocess.run", side_effect=fake_run):
                result = worker.cmd_powerOn({})
                panel = worker.cmd_getPanelId({})

            self.assertTrue(result["init_ok"])
            self.assertEqual(panel["panel_id"], "04830377")
            self.assertEqual(len(calls), 1)
            cmd, kwargs = calls[0]
            self.assertEqual(cmd[:3], [
                worker.python_exe,
                "-m",
                "ar_display_lab_lib.utilities.data_structures.hdf5_cal_file_updater",
            ])
            self.assertEqual(cmd[3], worker.cal_path)
            self.assertEqual(cmd[4], "04830377")
            self.assertEqual(cmd[5], "--get-defect-maps")
            self.assertTrue(cmd[6].endswith("defect_maps/04830377"))
            self.assertTrue(kwargs["capture_output"])
            self.assertTrue(kwargs["text"])

    def test_defect_map_export_failure_does_not_fail_power_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            worker = Worker()
            worker.cornea = FakeCornea()
            worker.cal_path = str(Path(tmp) / "hdf5_files")

            def fake_run(cmd, **kwargs):
                class Result:
                    returncode = 2
                    stdout = ""
                    stderr = "missing cal"

                return Result()

            with patch("panel_worker.subprocess.run", side_effect=fake_run):
                result = worker.cmd_powerOn({})

            self.assertTrue(result["init_ok"])
            self.assertFalse(result["defect_maps"]["ok"])
            self.assertIn("missing cal", result["defect_maps"]["error"])

    def test_empty_panel_id_blocks_power_on(self):
        worker = Worker()
        worker.cornea = FakeCornea(panel_id="")

        with self.assertRaisesRegex(RuntimeError, "empty Panel ID"):
            worker.cmd_powerOn({})

    def test_power_on_reports_sequence_failure_when_rax_lib_returns_init_not_ok(self):
        worker = Worker()
        worker.cornea = FakeCorneaPowerOnFailure()

        with self.assertRaisesRegex(RuntimeError, "Failure to sequence power supplies"):
            worker.cmd_powerOn({})

    def test_power_off_reports_failure_when_rax_lib_only_logs_cornea_power_down_error(self):
        worker = Worker()
        worker.cornea = FakeCorneaPowerOffFailure()

        with self.assertRaisesRegex(RuntimeError, "Failed to complete cornea_power_down"):
            worker.cmd_powerOff({})

    def test_power_off_capture_supports_logger_adapter(self):
        worker = Worker()
        worker.cornea = FakeCorneaPowerOffFailureWithLoggerAdapter()

        with self.assertRaisesRegex(RuntimeError, "Failed to complete cornea_power_down"):
            worker.cmd_powerOff({})


if __name__ == "__main__":
    unittest.main()
