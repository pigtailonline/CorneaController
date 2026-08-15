from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="ignore")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    return block_body(source, start)


def block_body(source: str, start: int) -> str:
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:i]
    raise AssertionError(f"block body not found from index {start}")


def call_expression(source: str, start: int) -> str:
    paren = source.index("(", start)
    depth = 0
    for i in range(paren, len(source)):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"call expression not closed from index {start}")


class MultiPanelWorkerArchitectureTests(unittest.TestCase):
    def test_block_body_reports_start_index_when_unclosed(self):
        with self.assertRaisesRegex(AssertionError, "block body not found from index 0"):
            block_body("{", 0)

    def test_usb_serialization_config_defaults_off_and_is_persisted(self):
        header = read("src/corneaconfig.h")
        source = read("src/corneaconfig.cpp")
        example = read("cornea_config.example.json")

        self.assertIn("bool serializeUsb = false;", header)
        self.assertIn('["serialize_usb"].toBool(false)', source)
        self.assertIn('["serialize_usb"] = m_python.serializeUsb', source)
        self.assertIn('"serialize_usb": false', example)

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
            '["defect_map_output_root"] = m_python.defectMapOutputRoot',
            config_cpp,
        )
        self.assertIn("setDefectMapOutputRoot", bridge_h)
        self.assertIn("m_defectMapOutputRoot", bridge_h)
        self.assertIn(
            '{"defect_map_output_root", m_defectMapOutputRoot}', bridge_cpp
        )
        self.assertIn(
            "setDefectMapOutputRoot(py.defectMapOutputRoot)", widget_cpp
        )
        self.assertIn('"defect_map_output_root": ""', example)

    def test_power_off_destroys_instance_between_duts(self):
        body = function_body(read("src/devicecontrolpanel.cpp"), "DeviceControlPanel::OpResult DeviceControlPanel::powerOffCore()")
        self.assertIn("m_controller->disconnect()", body)
        self.assertIn("!isConnected()", body)
        self.assertNotIn("m_controller->powerOff()", body)

    def test_power_on_binds_worker_by_expected_serial_not_only_index(self):
        controller_cpp = read("src/corneacontroller.cpp")
        bridge_cpp = read("src/pythonbridge.cpp")
        worker_py = read("python/panel_worker.py")
        connect_body = function_body(controller_cpp, "bool CorneaController::connect")
        create_call = call_expression(
            connect_body, connect_body.index("m_bridge->createDeviceInstance")
        )
        self.assertIn("deviceIndex", create_call)
        self.assertIn("hardwareVariant", create_call)
        self.assertIn("expectedSerial", create_call)
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

    def test_subprocess_init_stops_after_deterministic_hardware_failure(self):
        body = function_body(
            read("src/pythonbridge.cpp"), "int PythonBridge::createDeviceInstance"
        )
        self.assertIn('data.value("retryable").toBool(false)', body)
        self.assertIn('data.value("failure_kind")', body)
        self.assertIn('data.value("failure_reason")', body)
        self.assertIn("deterministic hardware failure", body)
        self.assertIn("if (!retryable)", body)
        non_retryable = body.index("if (!retryable)")
        self.assertIn("break;", body[non_retryable:non_retryable + 500])

    def test_subprocess_init_retries_only_allowlisted_transient_failures(self):
        body = function_body(
            read("src/pythonbridge.cpp"), "int PythonBridge::createDeviceInstance"
        )
        for marker in (
            "USB_BUSY_TIMEOUT",
            "USB_TRANSPORT_LOST",
            "SERIAL_NOT_FOUND",
            "No answer from FTDI",
            "subprocess exited",
            "timed out",
        ):
            self.assertIn(marker, body)
        self.assertIn("const bool retryable =", body)
        self.assertNotIn("const bool retryable = !deterministicHardwareFailure", body)

    def test_subprocess_retry_delay_only_runs_before_another_attempt(self):
        body = function_body(
            read("src/pythonbridge.cpp"), "int PythonBridge::createDeviceInstance"
        )
        self.assertIn("attempt < kSubprocessInitAttempts", body)
        self.assertNotIn("proc = nullptr;\n            QThread::msleep(1000);", body)

    def test_subprocess_failure_log_reports_actual_attempt_count(self):
        body = function_body(
            read("src/pythonbridge.cpp"), "int PythonBridge::createDeviceInstance"
        )
        self.assertIn("int attemptsMade = 0", body)
        self.assertIn("attemptsMade = attempt", body)
        self.assertIn(".arg(attemptsMade)", body)
        self.assertNotIn(".arg(kSubprocessInitAttempts)\n                    .arg(expectedSerial", body)

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

    def test_controller_lifecycle_errors_are_local_and_clear_when_stale(self):
        bridge_header = read("src/pythonbridge.h")
        bridge = read("src/pythonbridge.cpp")
        controller = read("src/corneacontroller.cpp")

        create_decl = call_expression(
            bridge_header, bridge_header.index("int createDeviceInstance")
        )
        self.assertIn("const QString &expectedSerial = QString()", create_decl)
        self.assertIn("QString *error = nullptr", create_decl)

        connect_body = function_body(controller, "bool CorneaController::connect")
        self.assertIn("QString commandError;", connect_body)
        create_call = call_expression(
            connect_body, connect_body.index("m_bridge->createDeviceInstance")
        )
        self.assertIn("deviceIndex", create_call)
        self.assertIn("hardwareVariant", create_call)
        self.assertIn("expectedSerial", create_call)
        self.assertIn("&commandError", create_call)
        self.assertIn("return failWithError(commandError", connect_body)
        self.assertIn("setLastError(QString());", connect_body)

        power_on = function_body(controller, "bool CorneaController::powerOn()")
        self.assertIn("QString commandError;", power_on)
        self.assertIn("systemPowerOn(m_instanceId, &commandError)", power_on)
        self.assertIn("return failWithError(commandError", power_on)
        self.assertIn("setLastError(QString());", power_on)

        precondition = function_body(
            controller, "bool CorneaController::requirePoweredOn"
        )
        self.assertEqual(2, precondition.count("return failWithError(err);"))

        disconnect = function_body(controller, "void CorneaController::disconnect()")
        power_off = function_body(controller, "bool CorneaController::powerOff()")
        self.assertIn("setLastError(QString());", disconnect)
        self.assertIn("setLastError(QString());", power_off)

        bridge_connect = function_body(
            bridge,
            "int PythonBridge::createDeviceInstance(int deviceIndex, const QString &hardwareVariant,\n"
            "                                       const QString &expectedSerial, QString *error)",
        )
        self.assertIn("if (error) *error = detail", bridge_connect)

        bridge_power_on = function_body(
            bridge, "bool PythonBridge::systemPowerOn(int instanceId, QString *error)"
        )
        self.assertIn("setInstanceError(instanceId, commandError)", bridge_power_on)
        self.assertIn("if (error) *error = commandError", bridge_power_on)
        self.assertIn("clearInstanceError(instanceId)", bridge_power_on)
        self.assertNotIn("getLastError()", bridge_power_on)

        brightness = function_body(controller, "bool CorneaController::setBrightness")
        image = function_body(controller, "bool CorneaController::sendImage")
        self.assertIn("setLastError(commandError)", brightness)
        self.assertIn("setLastError(commandError)", image)

    def test_shared_error_detail_synchronizes_worker_writes_and_caller_reads(self):
        source = read("src/pythonbridge.cpp")
        self.assertIn("class SharedErrorDetail", source)

        state = function_body(source, "class SharedErrorDetail")
        setter = function_body(state, "void set(const QString &detail)")
        getter = function_body(state, "QString get() const")
        self.assertIn("mutable QMutex m_mutex;", state)
        self.assertIn("QMutexLocker lock(&m_mutex);", setter)
        self.assertIn("m_detail = detail;", setter)
        self.assertIn("QMutexLocker lock(&m_mutex);", getter)
        self.assertIn("return m_detail;", getter)

    def test_invoke_state_synchronizes_result_publication_and_readback(self):
        source = read("src/pythonbridge.cpp")
        state = function_body(source, "struct InvokeState")
        invoke = function_body(source, "static auto invokeOnWorker")

        self.assertIn("void setResult(const Result &result)", state)
        publisher = function_body(state, "void setResult(const Result &result)")
        snapshot = function_body(state, "Result resultCopy() const")
        self.assertIn("mutable QMutex resultMutex;", state)
        self.assertIn("Result result{};", state)
        self.assertIn("QMutexLocker lock(&resultMutex);", publisher)
        self.assertIn("this->result = result;", publisher)
        self.assertIn("QMutexLocker lock(&resultMutex);", snapshot)
        self.assertIn("return result;", snapshot)

        self.assertNotIn("state->result =", invoke)
        self.assertNotIn("return state->result;", invoke)
        self.assertEqual(2, invoke.count("state->setResult(fn());"))
        self.assertEqual(2, invoke.count("return state->resultCopy();"))
        self.assertIn("state->done.tryAcquire(1, 15000)", invoke)
        self.assertLess(
            invoke.rindex("state->setResult(fn());"),
            invoke.index("state->done.release();"),
        )

    def test_embedded_create_keeps_caller_error_outside_dispatched_lambda(self):
        body = function_body(
            read("src/pythonbridge.cpp"),
            "int PythonBridge::createDeviceInstance(int deviceIndex, const QString &hardwareVariant,\n"
            "                                       const QString &expectedSerial, QString *error)",
        )
        dispatch_start = body.index("dispatch(")
        dispatch_call = call_expression(body, dispatch_start)
        capture_start = dispatch_call.index("[")
        capture_end = dispatch_call.index("]", capture_start) + 1
        capture = dispatch_call[capture_start:capture_end]
        dispatched_body = block_body(dispatch_call, capture_start)

        self.assertNotRegex(capture, r"\berror\b")
        self.assertNotIn("*error", dispatched_body)
        self.assertIn(
            "std::make_shared<SharedErrorDetail>()", body[:dispatch_start]
        )
        self.assertEqual(
            dispatched_body.count("setError("),
            dispatched_body.count("creationError->set("),
        )
        self.assertNotIn("*creationError", body)
        self.assertNotIn("creationError->isEmpty()", body)

        caller_copy = "if (error) *error = creationError->get();"
        self.assertIn(caller_copy, body)
        self.assertGreater(body.index(caller_copy), dispatch_start + len(dispatch_call))

    def test_embedded_command_error_details_are_synchronized_across_dispatch(self):
        source = read("src/pythonbridge.cpp")
        cases = (
            (
                "bool PythonBridge::systemPowerOn(int instanceId, QString *error)",
                "failureDetail",
            ),
            (
                "bool PythonBridge::setBrightness(int instanceId, double level, QString *error)",
                "fallbackDetail",
            ),
            (
                "bool PythonBridge::writeFrame(int instanceId, const QImage &image, QString *error)",
                "fallbackDetail",
            ),
        )

        for signature, state_name in cases:
            with self.subTest(signature=signature):
                body = function_body(source, signature)
                dispatch_start = body.index("dispatchToDevice(")
                dispatch_call = call_expression(body, dispatch_start)
                capture_start = dispatch_call.index("[")
                capture_end = dispatch_call.index("]", capture_start) + 1
                capture = dispatch_call[capture_start:capture_end]
                dispatched_body = block_body(dispatch_call, capture_start)
                caller_body = body[dispatch_start + len(dispatch_call) :]

                self.assertIn(state_name, capture)
                self.assertNotRegex(capture, r"\berror\b")
                self.assertIn(
                    "std::make_shared<SharedErrorDetail>()",
                    body[:dispatch_start],
                )
                self.assertIn(f"{state_name}->set(", dispatched_body)
                self.assertIn(f"{state_name}->get()", caller_body)
                self.assertNotIn(f"*{state_name}", body)
                self.assertNotIn(f"{state_name}->isEmpty()", body)

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

    def test_non_finite_worker_temperature_remains_invalid_across_bridge(self):
        worker = read("python/panel_worker.py")
        bridge = function_body(
            read("src/pythonbridge.cpp"),
            "double PythonBridge::getLeaTemperature(int instanceId)",
        )
        tcp = function_body(
            read("src/tcpserver.cpp"),
            "QJsonObject TcpServer::handleGetTemperature",
        )
        ui = function_body(
            read("src/devicecontrolpanel.cpp"),
            "void DeviceControlPanel::onControllerTemperatureUpdated",
        )

        self.assertIn("allow_nan=False", worker)
        self.assertIn('"non_finite": non_finite', worker)
        self.assertIn('data.value("non_finite").toBool()', bridge)
        self.assertIn("quiet_NaN()", bridge)
        self.assertIn("std::isfinite", tcp)
        self.assertIn("std::isfinite", ui)

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

    def test_embedded_brightness_requires_matching_readback(self):
        body = function_body(
            read("src/pythonbridge.cpp"),
            "bool PythonBridge::setBrightness(int instanceId, double level, QString *error)",
        )
        fallback = body[body.index("const auto fallbackDetail") :]

        self.assertIn('callInstanceMethod(instanceId, "set_brightness", args)', fallback)
        self.assertIn("result == Py_False", fallback)
        self.assertIn('callInstanceMethod(instanceId, "get_brightness")', fallback)
        self.assertIn("PyFloat_AsDouble", fallback)
        self.assertIn("std::isfinite(actual)", fallback)
        self.assertIn("qRound64(actual * 100.0) != qRound64(level * 100.0)", fallback)
        self.assertIn("BRIGHTNESS_SET_FAILED: get_brightness failed", fallback)
        self.assertIn("BRIGHTNESS_SET_FAILED: requested=%1, actual=%2", fallback)
        self.assertIn("setInstanceError(instanceId, commandError)", fallback)
        self.assertIn("if (error) *error = commandError", fallback)
        self.assertNotIn("markSubprocessUnhealthy", fallback)

    def test_powered_panel_error_handler_latches_only_while_powered(self):
        source = read("src/devicecontrolpanel.cpp")
        error_body = function_body(
            source,
            "void DeviceControlPanel::onControllerError(const QString &error)",
        )
        powered_guard_start = error_body.index("if (isPoweredOn())")
        powered_guard = block_body(error_body, powered_guard_start)

        self.assertIn('emit logMessage(panelLabel(), QString("ERROR: %1").arg(error));', error_body)
        self.assertEqual(error_body.count("m_commandError = true"), 1)
        self.assertNotIn("m_commandError = true", error_body[:powered_guard_start])
        self.assertIn("m_commandError = true", powered_guard)
        self.assertIn("updateUIState();", powered_guard)

    def test_powered_panel_command_error_status_precedes_normal_on_status(self):
        status_body = function_body(
            read("src/devicecontrolpanel.cpp"),
            "void DeviceControlPanel::updateUIState()",
        )
        command_error_branch = status_body.index(
            "} else if (poweredOn && m_commandError) {"
        )
        normal_on_branch = status_body.index("} else if (poweredOn) {")

        self.assertLess(command_error_branch, normal_on_branch)
        self.assertIn(
            'm_lblStatus->setText("On - Command Error")',
            status_body[command_error_branch:normal_on_branch],
        )
        self.assertIn(
            "color: darkorange; font-weight: bold;",
            status_body[command_error_branch:normal_on_branch],
        )

    def test_powered_panel_globally_confines_command_error_clears(self):
        header = read("src/devicecontrolpanel.h")
        source = read("src/devicecontrolpanel.cpp")
        clear = "m_commandError = false;"
        power_off_body = function_body(
            source,
            "DeviceControlPanel::OpResult DeviceControlPanel::powerOffCore()",
        )
        connected_body = function_body(
            source,
            "void DeviceControlPanel::onControllerConnected()",
        )
        disconnected_body = function_body(
            source,
            "void DeviceControlPanel::onControllerDisconnected()",
        )
        setup_body = function_body(source, "void DeviceControlPanel::setupConnections()")
        power_state_start = setup_body.index(
            "connect(m_controller.get(), &CorneaController::powerStateChanged"
        )
        power_state_body = block_body(setup_body, power_state_start)

        self.assertEqual(header.count(clear), 1)
        self.assertIn("m_controller->disconnect()", power_off_body)
        self.assertEqual(disconnected_body.count(clear), 1)
        self.assertEqual(connected_body.count(clear), 1)
        self.assertEqual(power_state_body.count(clear), 1)
        self.assertLess(
            disconnected_body.index(clear),
            disconnected_body.index("updateUIState();"),
        )
        self.assertLess(
            connected_body.index(clear),
            connected_body.index("updateUIState();"),
        )
        self.assertEqual(
            source.count(clear),
            disconnected_body.count(clear)
            + connected_body.count(clear)
            + power_state_body.count(clear),
        )
        self.assertLess(
            power_state_body.index(clear),
            power_state_body.index("if (poweredOn)"),
        )

    def test_powered_panel_invalid_rj1_feedback_preserves_unavailable_value(self):
        temperature_body = function_body(
            read("src/devicecontrolpanel.cpp"),
            "void DeviceControlPanel::onControllerTemperatureUpdated(double rj1, double da9272)",
        )
        invalid_definition_end = temperature_body.index("if (invalidRj1)")
        invalid_body = block_body(temperature_body, invalid_definition_end)
        valid_start = temperature_body.index("} else {", invalid_definition_end)
        valid_body = block_body(temperature_body, valid_start)
        valid_display_start = valid_body.index("m_lblRj1Temp->setText")
        valid_display = call_expression(valid_body, valid_display_start)

        self.assertIn("!std::isfinite(rj1)", temperature_body[:invalid_definition_end])
        self.assertIn(
            "(rj1 <= -30.0 && rj1 > -900.0)",
            temperature_body[:invalid_definition_end],
        )
        self.assertIn(
            "m_lblRj1Temp->setText(QString(\"Invalid (%1 C)\").arg(rj1, 0, 'f', 1));",
            invalid_body,
        )
        self.assertIn(
            "m_lblRj1Temp->setStyleSheet(\"color: red; font-weight: bold;\");",
            invalid_body,
        )
        self.assertIn("m_commandError = true;", invalid_body)
        self.assertIn("updateUIState();", invalid_body)
        self.assertLess(
            valid_body.index("m_lblRj1Temp->setStyleSheet(QString());"),
            valid_display_start,
        )
        self.assertEqual(
            valid_display,
            "m_lblRj1Temp->setText(rj1 <= -900.0\n"
            "            ? QStringLiteral(\"-\")\n"
            "            : QString(\"%1 C\").arg(rj1, 0, 'f', 1))",
        )
        self.assertNotIn("m_commandError = true", valid_body)


if __name__ == "__main__":
    unittest.main()
