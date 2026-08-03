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
    def test_usb_serialization_config_defaults_off_and_is_persisted(self):
        header = read("src/corneaconfig.h")
        source = read("src/corneaconfig.cpp")
        example = read("cornea_config.example.json")

        self.assertIn("bool serializeUsb = false;", header)
        self.assertIn('["serialize_usb"].toBool(false)', source)
        self.assertIn('["serialize_usb"] = m_python.serializeUsb', source)
        self.assertIn('"serialize_usb": false', example)

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
            "bool PythonBridge::writeFrame(int instanceId, const QImage &image, QString *error)",
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
        set_brightness_body = function_body(bridge_cpp, "bool PythonBridge::setBrightness(int instanceId, double level, QString *error)")
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

    def test_semantic_errors_are_instance_scoped_and_do_not_reconnect(self):
        bridge_header = read("src/pythonbridge.h")
        controller_header = read("src/corneacontroller.h")
        controller = read("src/corneacontroller.cpp")
        widget = read("src/corneawidget.cpp")

        self.assertIn("QString getLastError(int instanceId) const;", bridge_header)
        self.assertIn("QMap<int, QString> m_instanceLastErrors;", bridge_header)
        self.assertIn("QString m_lastError;", controller_header)
        self.assertIn("mutable QMutex m_lastErrorMutex;", controller_header)

        last_error = function_body(controller, "QString CorneaController::lastError() const")
        self.assertIn("m_lastErrorMutex", last_error)
        self.assertNotIn("m_bridge->getLastError()", last_error)

        set_brightness = function_body(controller, "bool CorneaController::setBrightness(double level)")
        self.assertIn("QString commandError;", set_brightness)
        self.assertIn("m_bridge->setBrightness(m_instanceId, level, &commandError)", set_brightness)

        serial_brightness = function_body(
            widget,
            "bool CorneaWidget::setBrightnessBySerial(const QString &serial, double level)",
        )
        semantic_branch = serial_brightness[
            serial_brightness.index('BRIGHTNESS_SET_FAILED'):
            serial_brightness.index("ctrl->disconnect()")
        ]
        self.assertIn("return false", semantic_branch)

    def test_invalid_temperature_replaces_the_tcp_temperature_observation(self):
        header = read("src/corneacontroller.h")
        body = function_body(
            read("src/corneacontroller.cpp"),
            "double CorneaController::getRj1Temperature()",
        )

        self.assertIn("bool m_hasRj1TemperatureObservation = false;", header)
        after_read = body[body.index("double temp = m_bridge->getLeaTemperature") :]
        self.assertIn("m_cachedRj1Temp = temp", after_read)
        self.assertIn("m_hasRj1TemperatureObservation = true", after_read)
        self.assertNotIn("return m_cachedRj1Temp;", after_read)

    def test_embedded_fallback_populates_command_local_errors(self):
        bridge = read("src/pythonbridge.cpp")
        brightness = function_body(
            bridge,
            "bool PythonBridge::setBrightness(int instanceId, double level, QString *error)",
        )
        frame = function_body(
            bridge,
            "bool PythonBridge::writeFrame(int instanceId, const QImage &image, QString *error)",
        )

        brightness_fallback = brightness
        self.assertIn("const bool success = dispatchToDevice", brightness_fallback)
        self.assertIn("BRIGHTNESS_SET_FAILED:", brightness_fallback)
        self.assertIn("setInstanceError(instanceId, commandError)", brightness_fallback)
        self.assertIn("if (error) *error = commandError", brightness_fallback)

        frame_fallback = frame
        self.assertIn("const bool success = dispatchToDevice", frame_fallback)
        self.assertIn("IMAGE_TRANSFER_FAILED:", frame_fallback)
        self.assertIn("setInstanceError(instanceId, commandError)", frame_fallback)
        self.assertIn("if (error) *error = commandError", frame_fallback)


if __name__ == "__main__":
    unittest.main()
