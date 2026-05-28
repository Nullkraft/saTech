"""Reusable full-test workflow for saTech hardware."""

from dataclasses import dataclass
import string
import time

import serial


DEFAULT_EXPECTED_ID = "saTech WN2A ready"


@dataclass
class FullTestConfig:
    port: str
    baud: int = 115200
    timeout: float = 1.0
    open_delay: float = 3.0
    expected_id: str = DEFAULT_EXPECTED_ID


@dataclass
class FullTestCheck:
    name: str
    expected: str
    actual: str
    passed: bool

    def to_dict(self):
        return {
            "name": self.name,
            "expected": self.expected,
            "actual": self.actual,
            "passed": self.passed,
        }


@dataclass
class FullTestStep:
    name: str
    command: str
    response: str
    response_hex: str

    def to_dict(self):
        return {
            "name": self.name,
            "command": self.command,
            "response": self.response,
            "response_hex": self.response_hex,
        }


@dataclass
class FullTestReport:
    unit_id: str
    steps: list
    checks: list
    raw_response_hex: str

    @property
    def passed(self):
        return all(check.passed for check in self.checks)

    def to_dict(self):
        return {
            "passed": self.passed,
            "unit_id": self.unit_id,
            "steps": [step.to_dict() for step in self.steps],
            "checks": [check.to_dict() for check in self.checks],
            "raw_response_hex": self.raw_response_hex,
        }


def run_full_test(config, serial_factory=serial.Serial):
    """Run the first full-test slice and return a structured report."""
    with serial_factory(config.port, config.baud, timeout=0.05) as ser:
        time.sleep(config.open_delay)
        ser.reset_input_buffer()
        id_step = _send_command(ser, "unit_id", "id", config.timeout)
        id_check = FullTestCheck(
            name="unit_id",
            expected=config.expected_id,
            actual=id_step.response,
            passed=config.expected_id in id_step.response,
        )
        if not id_check.passed:
            return FullTestReport(
                unit_id=id_step.response,
                steps=[id_step],
                checks=[id_check],
                raw_response_hex=id_step.response_hex,
            )
        set_ref1_step = _send_command(ser, "set_ref1", "set ref1", config.timeout)
        chip_off_step = _send_command(ser, "chip_off", "chip off", config.timeout)

    return FullTestReport(
        unit_id=id_step.response,
        steps=[id_step, set_ref1_step, chip_off_step],
        checks=[id_check],
        raw_response_hex=id_step.response_hex,
    )


def _send_command(ser, name, command, timeout):
    ser.write((command + "\n").encode("ascii"))
    ser.flush()
    response = _read_response(ser, timeout)
    return FullTestStep(
        name=name,
        command=command,
        response=_printable_text(response),
        response_hex=_hex_bytes(response),
    )


def _read_response(ser, timeout):
    deadline = time.monotonic() + timeout
    chunks = []

    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        if waiting:
            chunks.append(ser.read(waiting))
            deadline = time.monotonic() + 0.1
            continue
        time.sleep(0.01)

    return b"".join(chunks)


def _hex_bytes(data):
    return " ".join(f"{byte:02X}" for byte in data)


def _printable_text(data):
    text = data.decode("utf-8", errors="replace").strip()
    if not text:
        return ""
    printable = set(string.printable)
    return "".join(ch if ch in printable else "." for ch in text)
