from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="ignore")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:i]
    raise AssertionError(f"function body not found: {signature}")


class MultiPanelWorkerArchitectureTests(unittest.TestCase):
    def test_power_off_destroys_instance_between_duts(self):
        body = function_body(read("src/devicecontrolpanel.cpp"), "DeviceControlPanel::OpResult DeviceControlPanel::powerOffCore()")
        self.assertIn("m_controller->disconnect()", body)
        self.assertIn("!isConnected()", body)
        self.assertNotIn("m_controller->powerOff()", body)

    def test_power_on_binds_worker_by_expected_serial_not_only_index(self):
        controller_cpp = read("src/corneacontroller.cpp")
        bridge_cpp = read("src/pythonbridge.cpp")
        worker_py = read("python/panel_worker.py")
        self.assertIn("createDeviceInstance(deviceIndex, hardwareVariant, expectedSerial)", controller_cpp)
        self.assertIn('"cornea_serial"', bridge_cpp)
        self.assertIn("expectedSerial.isEmpty()", bridge_cpp)
        self.assertIn("actual_serial != expected_serial", worker_py)
        self.assertIn("SERIAL_MISMATCH", bridge_cpp)
        self.assertIn("SERIAL_NOT_FOUND", worker_py)

    def test_subprocess_usb_calls_take_one_global_gate(self):
        bridge_cpp = read("src/pythonbridge.cpp")
        worker_py = read("python/panel_worker.py")
        call_body = function_body(
            bridge_cpp,
            "QJsonObject PythonBridge::subprocessCall",
        )
        self.assertIn("LibusbGateLease", call_body)
        self.assertIn("USB_BUSY_TIMEOUT", call_body)
        self.assertIn("queue_wait_ms", call_body)
        self.assertIn("CorneaController_USB_GATE_v1", worker_py)
        self.assertIn("[USB-GATE]", worker_py)

    def test_temperature_poll_phase_does_not_overlap_panel_zero_and_five(self):
        body = function_body(
            read("src/devicecontrolpanel.cpp"),
            "void DeviceControlPanel::onControllerConnected()",
        )
        self.assertIn("kExpectedStationCount = 6", body)
        self.assertIn("/ kExpectedStationCount", body)
        self.assertNotIn("m_panelId * 1000", body)

    def test_manual_usb_refresh_never_blocks_ui_thread(self):
        body = function_body(
            read("src/corneawidget.cpp"),
            "void CorneaWidget::onRefreshDevicesClicked()",
        )
        self.assertIn("QtConcurrent::run", body)
        self.assertIn("applyDeviceList", body)
        self.assertNotIn("refreshDeviceList()", body)

    def test_transport_failure_isolates_only_failed_send_image_worker(self):
        body = function_body(
            read("src/pythonbridge.cpp"),
            "bool PythonBridge::writeFrame(int instanceId, const QImage &image)",
        )
        self.assertIn('markSubprocessUnhealthy(instanceId, "sendImage"', body)
        self.assertIn("USB_BUSY_TIMEOUT", body)

    def test_tcp_errors_expose_stable_usb_error_codes(self):
        body = function_body(
            read("src/tcpserver.cpp"),
            "QJsonObject TcpServer::makeError(const QString &message)",
        )
        for code in (
            "USB_BUSY_TIMEOUT",
            "USB_TRANSPORT_LOST",
            "SERIAL_NOT_FOUND",
            "SERIAL_MISMATCH",
        ):
            self.assertIn(code, body)

    def test_subprocess_init_retries_before_failing_connect(self):
        body = function_body(read("src/pythonbridge.cpp"), "int PythonBridge::createDeviceInstance")
        self.assertIn("kSubprocessInitAttempts", body)
        self.assertIn("attempt", body)
        self.assertIn("init failed on attempt", body)

    def test_subprocess_worker_is_reused_but_hardware_instance_is_reset(self):
        bridge_cpp = read("src/pythonbridge.cpp")
        destroy_body = function_body(bridge_cpp, "void PythonBridge::destroyDeviceInstance(int instanceId)")
        create_body = function_body(bridge_cpp, "int PythonBridge::createDeviceInstance")
        worker_py = read("python/panel_worker.py")

        self.assertIn("cmd_reset", worker_py)
        self.assertIn('"reset"', worker_py)
        self.assertIn('proc->sendBlocking("reset"', destroy_body)
        self.assertIn("m_idlePanelProcs.insert", destroy_body)
        self.assertNotIn("proc->stop(5000)", destroy_body)
        self.assertIn("m_idlePanelProcs.take", create_body)
        self.assertIn("Reusing idle subprocess", create_body)

    def test_failed_subprocess_command_marks_worker_unhealthy(self):
        bridge_cpp = read("src/pythonbridge.cpp")
        self.assertIn("markSubprocessUnhealthy", bridge_cpp)
        self.assertIn('markSubprocessUnhealthy(instanceId, "setBrightness"', bridge_cpp)
        set_brightness_body = function_body(bridge_cpp, "bool PythonBridge::setBrightness(int instanceId, double level)")
        self.assertIn("markSubprocessUnhealthy", set_brightness_body)
        self.assertIn("subprocessCall", set_brightness_body)

    def test_tcp_set_brightness_reconnects_once_after_driver_failure(self):
        body = function_body(read("src/corneawidget.cpp"), "bool CorneaWidget::setBrightnessBySerial(const QString &serial, double level)")
        self.assertIn("ctrl->disconnect()", body)
        self.assertIn("ctrl->connect(panel->deviceIndex(), panel->currentVariant(), serial)", body)
        self.assertIn("retry", body)
        self.assertIn("USB_BUSY_TIMEOUT", body)
        self.assertIn("not reconnecting", body)

    def test_tcp_power_off_by_serial_destroys_instance_between_duts(self):
        body = function_body(read("src/corneawidget.cpp"), "bool CorneaWidget::powerOffBySerial(const QString &serial)")
        self.assertIn("ctrl->disconnect()", body)
        self.assertIn("return !ctrl->isConnected()", body)
        self.assertNotIn("ctrl->powerOff()", body)

    def test_tcp_power_on_by_serial_uses_expected_serial_on_fresh_connect(self):
        body = function_body(read("src/corneawidget.cpp"), "bool CorneaWidget::powerOnBySerial(const QString &serial, const QString &variant)")
        self.assertIn("ctrl->connect(panel->deviceIndex(), variant, serial)", body)


if __name__ == "__main__":
    unittest.main()
