"""Reusable full-test workflow for saTech hardware."""

from dataclasses import dataclass, field
import json
import math
import string
import time

import serial


DEFAULT_EXPECTED_ID = "saTech WN2A ready"
STARTUP_RF_MHZ = 1735.113
REF_CLOCK_MHZ = 66.0
IF1_CENTER_MHZ = 3600.0
IF2_MHZ = 315.0


@dataclass
class FullTestConfig:
    port: str
    baud: int = 115200
    timeout: float = 1.0
    open_delay: float = 3.0
    expected_id: str = DEFAULT_EXPECTED_ID
    atten_db: float = 12.0
    rfin_mhz: float = 10.0
    lo_off_target_mhz: float = 123.345


@dataclass
class FullTestCheck:
    name: str
    expected: object
    actual: object
    passed: bool

    def to_dict(self):
        return {
            "name": self.name,
            "expected": self.expected,
            "actual": self.actual,
            "passed": self.passed,
        }


@dataclass
class FullTestValue:
    name: str
    value: object

    def to_dict(self):
        return {
            "name": self.name,
            "value": self.value,
        }


@dataclass
class FullTestStep:
    name: str
    command: str
    response: str
    response_hex: str


@dataclass
class FullTestRegister:
    lo: str
    register: str
    state: str
    expected: object
    decoded: object
    result: str

    def to_dict(self):
        return {
            "lo": self.lo,
            "register": self.register,
            "state": self.state,
            "expected": self.expected,
            "decoded": self.decoded,
            "result": self.result,
        }


@dataclass
class FullTestReport:
    unit_id: str
    steps: list
    checks: list
    values: list
    registers: list = field(default_factory=list)

    @property
    def passed(self):
        return all(check.passed for check in self.checks)

    def to_dict(self):
        result = {
            "passed": self.passed,
            "unit_id": self.unit_id,
            "commands": [step.command for step in self.steps],
            "checks": [check.to_dict() for check in self.checks],
            "values": [value.to_dict() for value in self.values],
        }
        if self.registers:
            result["registers"] = [register.to_dict() for register in self.registers]
        return result


def run_full_test(config, serial_factory=serial.Serial, rigol=None, meter=None, expected_register_provider=None):
    """Run the first full-test slice and return a structured report."""
    if rigol is not None and expected_register_provider is None:
        expected_register_provider = _default_expected_register_provider

    with serial_factory(config.port, config.baud, timeout=0.05) as ser:
        time.sleep(config.open_delay)
        ser.reset_input_buffer()
        _drain_serial_until_quiet(ser)
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
                values=[],
            )
        refcheck_step = _send_command(ser, "refcheck", "fulltest refcheck", config.timeout)
        set_ref1_step = _send_command(ser, "set_ref1", "set ref1", config.timeout)
        pincheck_step = _send_command(ser, "pincheck", "fulltest pincheck", config.timeout)
        chip_off_step = _send_command(ser, "chip_off", "chip off", config.timeout)
        atten_step = _send_command(ser, "atten", f"fulltest atten {config.atten_db:.2f}", config.timeout)

        steps = [
            id_step,
            refcheck_step,
            set_ref1_step,
            pincheck_step,
            chip_off_step,
            atten_step,
        ]
        json_steps = [
            refcheck_step,
            pincheck_step,
            atten_step,
        ]
        register_checks = []
        registers = []

        if rigol is not None:
            _call_scope_method(rigol, "scope_setup", "setup", "rigol_ds1102e_scope_setup")
            steps.extend(_prime_lo_for_capture(ser, "lo1", config.lo_off_target_mhz, config.timeout))
            steps.extend(_prime_lo_for_capture(ser, "lo2", config.lo_off_target_mhz, config.timeout))
            steps.append(_send_command(ser, "chip_off_after_prime", "chip off", config.timeout))

        plan_step = _send_command(ser, "plan", f"fulltest plan {config.rfin_mhz:.3f}", config.timeout)
        steps.append(plan_step)
        json_steps.append(plan_step)

        measurement_values = []

        if rigol is not None:
            lo1_frequency = _value_from_json_step(plan_step, "lo1_frequency_mhz")
            lo2_frequency = _value_from_json_step(plan_step, "lo2_frequency_mhz")

            program_lo1_step, lo1_checks, lo1_registers = _program_and_verify_lo(
                ser,
                rigol,
                "lo1",
                lo1_frequency,
                config.lo_off_target_mhz,
                config.timeout,
                expected_register_provider,
            )
            steps.append(program_lo1_step)
            json_steps.append(program_lo1_step)
            register_checks.extend(lo1_checks)
            registers.extend(lo1_registers)
            if not _checks_passed(lo1_checks):
                return _build_report(id_step.response, steps, id_check, json_steps, register_checks, registers)

            program_lo2_step, lo2_checks, lo2_registers = _program_and_verify_lo(
                ser,
                rigol,
                "lo2",
                lo2_frequency,
                config.lo_off_target_mhz,
                config.timeout,
                expected_register_provider,
            )
            steps.append(program_lo2_step)
            json_steps.append(program_lo2_step)
            register_checks.extend(lo2_checks)
            registers.extend(lo2_registers)
            if not _checks_passed(lo2_checks):
                return _build_report(id_step.response, steps, id_check, json_steps, register_checks, registers)
        else:
            program_lo1_step = _send_command(ser, "program_lo1", "fulltest program lo1", config.timeout)
            program_lo2_step = _send_command(ser, "program_lo2", "fulltest program lo2", config.timeout)
            steps.extend([program_lo1_step, program_lo2_step])
            json_steps.extend([program_lo1_step, program_lo2_step])

        if meter is not None:
            steps.append(_send_command(ser, "chip_off_final", "chip off", config.timeout))
            measurement_values.extend(_measure_path_output(meter))

    return _build_report(id_step.response, steps, id_check, json_steps, register_checks, registers, measurement_values)


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


def _program_and_verify_lo(ser, rigol, lo_name, frequency_mhz, previous_frequency_mhz, timeout, expected_register_provider):
    expected = _expected_registers(expected_register_provider, lo_name, frequency_mhz, previous_frequency_mhz)
    expected_addresses = [word["address"] for word in _register_words(expected)]
    _call_scope_method(rigol, "start_new_waveform", "rigol_start_new_waveform")
    step = _send_command(ser, f"program_{lo_name}", f"fulltest program {lo_name}", timeout)
    try:
        capture = _call_scope_method(rigol, "capture_waveform", "capture_waveform_channels")
        decoded = _call_scope_method(
            rigol,
            "spi_decode",
            "rigol_ds1102e_spi_decode",
            capture,
            expected_writes=len(expected_addresses),
            expected_addresses=expected_addresses,
        )
    except Exception as exc:
        checks = [_register_summary_check(lo_name, False, f"decode_error: {type(exc).__name__}: {exc}")]
        registers = _register_records(lo_name, expected, expected_addresses, {})
        return step, checks, registers
    checks, registers = _verify_registers(lo_name, expected, decoded)
    return step, checks, registers


def _build_report(unit_id, steps, id_check, json_steps, register_checks, registers, measurement_values=None):
    checks = [id_check]
    for step in json_steps:
        checks.extend(_checks_from_json_step(step))
    checks.extend(register_checks)
    values = []
    for step in json_steps:
        values.extend(_values_from_json_step(step))
    if measurement_values is not None:
        values.extend(measurement_values)
    return FullTestReport(
        unit_id=unit_id,
        steps=steps,
        checks=checks,
        values=values,
        registers=registers,
    )


def _prime_lo_for_capture(ser, lo_name, frequency_mhz, timeout):
    return [
        _send_command(ser, f"prime_{lo_name}_select", f"chip {lo_name}", timeout),
        _send_command(ser, f"prime_{lo_name}_frequency", f"lofreq {frequency_mhz:.3f}", timeout),
    ]


def _default_expected_register_provider(lo_name, frequency_mhz, previous_frequency_mhz=None):
    from max2871_expected import expected_registers_for_frequencies

    if previous_frequency_mhz is None:
        previous_frequency_mhz = _startup_lo_frequency(lo_name)
    return expected_registers_for_frequencies([previous_frequency_mhz, frequency_mhz])[-1]


def _expected_registers(provider, lo_name, frequency_mhz, previous_frequency_mhz=None):
    try:
        return provider(lo_name, frequency_mhz, previous_frequency_mhz)
    except TypeError:
        try:
            return provider(lo_name, frequency_mhz)
        except TypeError:
            return provider(frequency_mhz)


def _startup_lo_frequency(lo_name):
    lo1_frequency, lo2_frequency = _planned_lo_frequencies(STARTUP_RF_MHZ)
    if lo_name == "lo1":
        return lo1_frequency
    if lo_name == "lo2":
        return lo2_frequency
    raise ValueError(f"unsupported LO {lo_name}")


def _planned_lo_frequencies(rfin_mhz):
    fpfd = REF_CLOCK_MHZ
    if1_step = fpfd * math.floor((IF1_CENTER_MHZ / fpfd) + 0.5)
    hi_lo1 = rfin_mhz < 2343.0001
    sign = 1 if hi_lo1 else -1
    lo1_frequency = fpfd * math.floor(((if1_step + sign * rfin_mhz) / fpfd) + 0.5)
    if1 = abs(lo1_frequency - sign * rfin_mhz)
    lo2_frequency = if1 + IF2_MHZ
    return lo1_frequency, lo2_frequency


def _call_scope_method(scope, *names, **kwargs):
    args = ()
    if names and not isinstance(names[-1], str):
        args = (names[-1],)
        names = names[:-1]
    for name in names:
        method = getattr(scope, name, None)
        if method is not None:
            return method(*args, **kwargs)
    raise AttributeError(f"scope does not provide any of: {', '.join(names)}")


def _checks_passed(checks):
    return all(check.passed for check in checks)


def _verify_registers(lo_name, expected, decoded):
    expected_words = _register_words(expected)
    decoded_words = _register_words(decoded)
    expected_addresses = [word["address"] for word in expected_words]
    decoded_addresses = [word["address"] for word in decoded_words]
    expected_map = _register_map(expected_words)
    decoded_map = _register_map(decoded_words)
    registers = _register_records(lo_name, expected, expected_addresses, decoded_map)
    checks = []

    count_passed = len(decoded_words) == len(expected_words)
    checks.append(FullTestCheck(
        name=f"{lo_name}_register_count",
        expected=len(expected_words),
        actual=len(decoded_words),
        passed=count_passed,
    ))
    if not count_passed:
        checks.append(_register_summary_check(lo_name, False, "register_count"))
        return checks, registers

    addresses_passed = decoded_addresses == expected_addresses
    checks.append(FullTestCheck(
        name=f"{lo_name}_register_addresses",
        expected=expected_addresses,
        actual=decoded_addresses,
        passed=addresses_passed,
    ))
    if not addresses_passed:
        checks.append(_register_summary_check(lo_name, False, "register_addresses"))
        return checks, registers

    values_passed = True
    for address in expected_addresses:
        expected_hex = expected_map.get(address)
        decoded_hex = decoded_map.get(address)
        passed = expected_hex == decoded_hex
        checks.append(FullTestCheck(
            name=f"{lo_name}_r{address}",
            expected=expected_hex,
            actual=decoded_hex,
            passed=passed,
        ))
        if not passed:
            values_passed = False
            break

    checks.append(_register_summary_check(lo_name, values_passed, "PASS" if values_passed else "register_values"))
    return checks, registers


def _register_summary_check(lo_name, passed, detail):
    return FullTestCheck(
        name=f"{lo_name}_register_verification",
        expected="PASS",
        actual="PASS" if passed else detail,
        passed=passed,
    )


def _register_records(lo_name, expected, dirty_addresses, decoded_map):
    expected_registers = expected.get("registers", {})
    records = []
    for address in range(5, -1, -1):
        expected_hex = _hex_value(expected_registers.get(address))
        decoded_hex = decoded_map.get(address)
        state = "DIRTY" if address in dirty_addresses else "CLEAN"
        if state == "DIRTY" and decoded_hex == expected_hex:
            result = "PASS"
        elif state == "DIRTY" and decoded_hex is None:
            result = "MISSING"
        elif state == "DIRTY":
            result = "FAIL"
        elif decoded_hex is None:
            result = "CLEAN"
        else:
            result = "UNEXPECTED_WRITE"
        records.append(FullTestRegister(
            lo=lo_name.upper(),
            register=f"R{address}",
            state=state,
            expected=expected_hex,
            decoded=decoded_hex,
            result=result,
        ))
    return records


def _register_words(register_data):
    if isinstance(register_data, list):
        return [_register_word_from_value(value, None) for value in register_data]
    for key in ("writes", "decoded_words"):
        if key in register_data:
            return [_register_word_from_mapping(word, index) for index, word in enumerate(register_data[key])]
    if "address_map" in register_data:
        words = []
        for address, value in register_data["address_map"].items():
            if value is not None:
                words.append(_register_word_from_value(value, int(address)))
        return words
    return []


def _register_word_from_mapping(word, index):
    value = word.get("value", word.get("hex"))
    if value is None:
        value = word.get("decoded")
    address = word.get("address")
    return _register_word_from_value(value, int(address) if address is not None else None)


def _register_word_from_value(value, fallback_address):
    numeric_value = int(value, 16) if isinstance(value, str) else int(value)
    return {
        "address": numeric_value & 0x7 if fallback_address is None else int(fallback_address),
        "value": numeric_value,
        "hex": f"0x{numeric_value:08X}",
    }


def _register_map(words):
    return {word["address"]: word["hex"] for word in words}


def _hex_value(value):
    if value is None:
        return None
    if isinstance(value, str):
        return f"0x{int(value, 16):08X}"
    return f"0x{int(value):08X}"


def _value_from_json_step(step, name):
    for value in _values_from_json_step(step):
        if value.name == name:
            return value.value
    raise ValueError(f"{name} was not reported by {step.command}")


def _measure_path_output(meter):
    reading = None
    for _ in range(3):
        reading = _call_meter_method(meter, "read_measurement", "bk390a_read")
    voltage_v = _measurement_voltage_volts(reading["measurement"])
    power_dbm = _volts_to_dbm(voltage_v)
    return [
        FullTestValue(name="bk390a_voltage_v", value=voltage_v),
        FullTestValue(name="logamp_power_dbm", value=power_dbm),
    ]


def _call_meter_method(meter, *names, **kwargs):
    for name in names:
        method = getattr(meter, name, None)
        if method is not None:
            return method(**kwargs)
    raise AttributeError(f"meter does not provide any of: {', '.join(names)}")


def _measurement_voltage_volts(measurement):
    value = measurement["value"]
    unit = measurement["unit"]
    if unit == "V":
        return value
    if unit == "mV":
        return value / 1000.0
    raise ValueError(f"unsupported BK390A unit {unit!r}")


def _volts_to_dbm(voltage):
    x = voltage
    return (((((((-9.460927 * x + 110.57352) * x - 538.8610489) * x + 1423.9059205) * x - 2219.08322) * x + 2073.3123) * x - 1122.5121) * x + 355.7665) * x - 112.663


def _checks_from_json_step(step):
    checks = []
    for line in step.response.splitlines():
        if not line.startswith("{"):
            continue
        record = json.loads(line)
        if record.get("type") != "check":
            continue
        checks.append(
            FullTestCheck(
                name=record["name"],
                expected=record["expected"],
                actual=record["actual"],
                passed=record["result"] == "PASS",
            )
        )
    return checks


def _values_from_json_step(step):
    values = []
    for line in step.response.splitlines():
        if not line.startswith("{"):
            continue
        record = json.loads(line)
        if record.get("type") != "value":
            continue
        values.append(FullTestValue(name=record["name"], value=record["value"]))
    return values


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


def _drain_serial_until_quiet(ser, timeout=8.0, quiet_period=0.5):
    deadline = time.monotonic() + timeout
    quiet_deadline = time.monotonic() + quiet_period
    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        if waiting:
            ser.read(waiting)
            quiet_deadline = time.monotonic() + quiet_period
            continue
        if time.monotonic() >= quiet_deadline:
            return
        time.sleep(0.01)


def _hex_bytes(data):
    return " ".join(f"{byte:02X}" for byte in data)


def _printable_text(data):
    text = data.decode("utf-8", errors="replace").strip()
    if not text:
        return ""
    printable = set(string.printable)
    return "".join(ch if ch in printable else "." for ch in text)
