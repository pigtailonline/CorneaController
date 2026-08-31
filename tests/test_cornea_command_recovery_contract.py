"""Deterministic contracts for recover-cornea-usb-transport-fault.

These tests intentionally do not open USB devices or require Qt.  The source
contracts catch accidental removal of the production protocol/state guards,
while the tiny model exercises the deadline/generation invariants that are
otherwise only observable on a station with six physical panels.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8", errors="ignore")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unclosed function: {signature}")


@dataclass
class _Request:
    serial: str
    generation: int
    deadline_ms: int
    active: bool = True


class _RecoveryModel:
    """Small executable model of the required result-publication rules."""

    def __init__(self, serials: list[str], deadline_ms: int = 14_000):
        self.state = {serial: "Ready" for serial in serials}
        self.generation = {serial: 1 for serial in serials}
        self.deadline_ms = deadline_ms

    def request(self, serial: str, now_ms: int) -> _Request | str:
        if self.state[serial] == "Recovering":
            return "RECOVERY_IN_PROGRESS"
        if self.state[serial] == "Unavailable":
            return "RECOVERY_FAILED"
        return _Request(serial, self.generation[serial], now_ms + self.deadline_ms)

    def quarantine(self, serial: str) -> None:
        self.state[serial] = "Recovering"
        self.generation[serial] += 1

    def finish_recovery(self, serial: str, available: bool) -> None:
        self.state[serial] = "Ready" if available else "Unavailable"
        if available:
            self.generation[serial] += 1

    def publish(self, request: _Request, generation: int, now_ms: int) -> str:
        if not request.active or now_ms >= request.deadline_ms:
            request.active = False
            return "COMMAND_TIMEOUT"
        if generation != self.generation[request.serial]:
            request.active = False
            return "STALE_GENERATION"
        request.active = False
        return "PUBLISHED"


class RecoverySourceContractTests(unittest.TestCase):
    def test_api_result_keeps_legacy_fields_and_adds_optional_metadata(self):
        header = read("src/corneawidget.h")
        struct = header[header.index("struct ApiResult") : header.index("};", header.index("struct ApiResult"))]
        self.assertIn("bool success", struct)
        self.assertIn("QString error", struct)
        for field in ("errorCode", "requestId", "timeoutMs", "deviceState"):
            self.assertIn(field, struct)
        self.assertIn("static ApiResult ok", struct)
        self.assertIn("static ApiResult fail", struct)

    def test_request_id_is_preserved_or_generated_and_echoed(self):
        tcp = read("src/tcpserver.cpp")
        self.assertIn("ensureRequestId", tcp)
        self.assertIn('command[QStringLiteral("requestId")] = requestId', tcp)
        self.assertGreaterEqual(tcp.count('response["requestId"] = requestId'), 2)
        self.assertIn("requestId, isExclusive", tcp)

    def test_power_failures_have_structured_timeout_and_recovery_fields(self):
        tcp = read("src/tcpserver.cpp")
        for code in ("COMMAND_TIMEOUT", "RECOVERY_IN_PROGRESS", "RECOVERY_FAILED"):
            self.assertIn(code, tcp)
        for field in ("timeout_ms", "deviceState", "errorCode"):
            self.assertIn(f'response["{field}"]', tcp)
        self.assertIn("powerOnBySerialEx", tcp)
        self.assertIn("powerOffBySerialEx", tcp)

    def test_status_adds_device_state_without_removing_legacy_fields(self):
        body = function_body(read("src/tcpserver.cpp"), "QJsonObject TcpServer::handleGetStatus")
        for field in ("serial", "connected", "panelId", "variant"):
            self.assertIn(f'dev["{field}"]', body)
        self.assertIn('dev["deviceState"]', body)
        self.assertIn('dev["sessionState"]', body)

    def test_power_deadline_is_driver_owned_and_below_client_boundary(self):
        header = read("src/pythonbridge.h")
        self.assertIn("DEFAULT_POWER_DEADLINE_MS = 14000", header)
        self.assertIn("DEFAULT_RECOVERY_BUDGET_MS", header)
        self.assertIn("m_powerDeadlineMs", header)
        self.assertIn("m_recoveryBudgetMs", header)
        self.assertIn("m_powerDeadlineMs = qBound", header)
        bridge = read("src/pythonbridge.cpp")
        power_on = function_body(bridge, "bool PythonBridge::systemPowerOn")
        power_off = function_body(bridge, "bool PythonBridge::systemPowerOff")
        self.assertIn("powerDeadlineMs()", power_on)
        self.assertIn("powerDeadlineMs()", power_off)
        self.assertNotIn('subprocessCall(instanceId, "powerOn", {}, 60000)', power_on)
        self.assertNotIn('subprocessCall(instanceId, "powerOff", {}, 30000)', power_off)

    def test_session_coordinator_is_per_serial_and_generation_based(self):
        header = read("src/pythonbridge.h")
        for state in ("Ready", "Recovering", "Unavailable"):
            self.assertIn(state, header)
        self.assertIn("workerGeneration", header)
        self.assertIn("QMap<QString, SessionCoordinator> m_sessions", header)
        self.assertIn("QMap<int, InstanceMetadata> m_instanceMetadata", header)
        bridge = read("src/pythonbridge.cpp")
        self.assertIn("++session.generation", bridge)
        self.assertIn("sessionStateChanged", bridge)

    def test_recovery_uses_actual_serial_when_initial_connect_was_index_only(self):
        bridge = read("src/pythonbridge.cpp")
        metadata = bridge[bridge.index("InstanceMetadata &metadata", bridge.index("m_initOkMap[instanceId] = initOk")) :]
        self.assertIn("expectedSerial.isEmpty()", metadata)
        self.assertIn("m_serialMap.value(instanceId)", metadata)
        recover = function_body(bridge, "bool PythonBridge::recoverDeviceInstance")
        self.assertIn('"cornea_serial", metadata.expectedSerial', recover)
        self.assertIn("actualSerial == metadata.expectedSerial", recover)

    def test_only_allowlisted_transport_failures_start_recovery(self):
        bridge = read("src/pythonbridge.cpp")
        classifier = function_body(bridge, "bool PythonBridge::isRetryableTransportFailure")
        for code in ("USB_TRANSPORT_LOST", "WORKER_EXIT", "WORKER_UNAVAILABLE"):
            self.assertIn(code, classifier)
        for terminal in ("calibration", "ucid", "serial_mismatch", "argument", "device_operation_failed"):
            self.assertIn(terminal, classifier)
        self.assertIn("markSubprocessUnhealthy", bridge)

    def test_retry_path_has_one_recovery_and_one_second_call(self):
        bridge = read("src/pythonbridge.cpp")
        body = function_body(bridge, "QJsonObject PythonBridge::subprocessCallWithRecovery")
        self.assertEqual(2, body.count("subprocessCall(instanceId, cmd, args"))
        self.assertIn("recoverDeviceInstance", body)
        self.assertIn("stale completion discarded", body)
        self.assertIn("recoveryBudgetMs()", body)

    def test_worker_cleanup_is_bounded(self):
        bridge = read("src/pythonbridge.cpp")
        recovery = function_body(bridge, "bool PythonBridge::recoverDeviceInstance")
        quarantine = function_body(bridge, "void PythonBridge::quarantineSubprocess")
        self.assertRegex(recovery, r"proc->spawn\(qMin\(5000")
        self.assertRegex(recovery, r"proc->stop\(qMin\(1000")
        self.assertIn("proc->stop(1000)", quarantine)
        subprocess = read("src/panelsubprocess.cpp")
        self.assertIn("cleanupClock", subprocess)
        self.assertIn("remainingMs", subprocess)

    def test_power_state_and_ui_unknown_contract(self):
        controller = read("src/corneacontroller.h") + read("src/corneacontroller.cpp")
        for state in ("PowerState::Off", "PowerState::On", "PowerState::Unknown"):
            self.assertIn(state, controller)
        self.assertIn("isPoweredOn() const { return m_powerState == PowerState::On; }", controller)
        panel = read("src/devicecontrolpanel.cpp")
        self.assertIn('"Unknown - Command Error"', panel)
        self.assertIn("completePowerOperation", panel)
        self.assertIn("std::atomic<bool>", panel)
        self.assertIn("UI watchdog", panel)


class RecoveryModelTests(unittest.TestCase):
    def test_recovering_serial_rejects_new_work_but_other_serial_is_ready(self):
        model = _RecoveryModel(["A", "B", "C", "D", "E", "F"])
        model.quarantine("A")
        self.assertEqual("RECOVERY_IN_PROGRESS", model.request("A", 100))
        self.assertIsInstance(model.request("B", 100), _Request)

    def test_unavailable_serial_returns_structured_failure_without_retry(self):
        model = _RecoveryModel(["A"])
        model.quarantine("A")
        model.finish_recovery("A", available=False)
        self.assertEqual("RECOVERY_FAILED", model.request("A", 100))

    def test_expired_result_cannot_publish_after_recovery(self):
        model = _RecoveryModel(["A"])
        request = model.request("A", 0)
        self.assertIsInstance(request, _Request)
        model.quarantine("A")
        model.finish_recovery("A", available=True)
        self.assertEqual("STALE_GENERATION", model.publish(request, request.generation, 100))

    def test_expired_result_returns_timeout_before_client_boundary(self):
        model = _RecoveryModel(["A"], deadline_ms=14_000)
        request = model.request("A", 0)
        self.assertIsInstance(request, _Request)
        self.assertEqual("COMMAND_TIMEOUT", model.publish(request, request.generation, 14_000))
        self.assertLess(request.deadline_ms, 15_000)

    def test_generation_is_monotonic_across_quarantine_and_recovery(self):
        model = _RecoveryModel(["A"])
        first = model.generation["A"]
        model.quarantine("A")
        second = model.generation["A"]
        model.finish_recovery("A", available=True)
        third = model.generation["A"]
        self.assertLess(first, second)
        self.assertLess(second, third)


if __name__ == "__main__":
    unittest.main()
