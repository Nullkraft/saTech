import unittest

from rigol_full_test_adapter import RigolFullTestAdapter
from run_full_test import FullTestConfig, run_full_test


REFCHECK_RESPONSE = (
    b'{"type":"report_begin","report":"refcheck"}\r\n'
    b'{"type":"check","name":"ref1_selected","expected":"Ref1 on : Ref2 off","actual":"Ref1 on : Ref2 off","result":"PASS"}\r\n'
    b'{"type":"check","name":"ref2_selected","expected":"Ref1 off : Ref2 on","actual":"Ref1 off : Ref2 on","result":"PASS"}\r\n'
    b'{"type":"check","name":"refs_off","expected":"Ref1 off : Ref2 off","actual":"Ref1 off : Ref2 off","result":"PASS"}\r\n'
    b'{"type":"report_end","report":"refcheck"}\r\n'
)

ATTEN_RESPONSE = (
    b'{"type":"report_begin","report":"atten"}\r\n'
    b'{"type":"value","name":"atten_db","value":12.00}\r\n'
    b'{"type":"report_end","report":"atten"}\r\n'
)

PINCHECK_RESPONSE = (
    b'{"type":"report_begin","report":"pincheck"}\r\n'
    b'{"type":"check","name":"pin_checks","expected":"PASS","actual":"PASS","result":"PASS"}\r\n'
    b'{"type":"report_end","report":"pincheck"}\r\n'
)

PLAN_RESPONSE = (
    b'{"type":"report_begin","report":"plan"}\r\n'
    b'{"type":"value","name":"rfin_mhz","value":10.000}\r\n'
    b'{"type":"value","name":"if1_mhz","value":3620.000}\r\n'
    b'{"type":"value","name":"if2_mhz","value":315.000}\r\n'
    b'{"type":"value","name":"lo1_frequency_mhz","value":3630.000}\r\n'
    b'{"type":"check","name":"lo1_frequency_mhz","expected":3630.000,"actual":3630.000,"result":"PASS"}\r\n'
    b'{"type":"value","name":"lo2_frequency_mhz","value":3935.000}\r\n'
    b'{"type":"check","name":"lo2_frequency_mhz","expected":3935.000,"actual":3935.000,"result":"PASS"}\r\n'
    b'{"type":"value","name":"atten_db","value":12.00}\r\n'
    b'{"type":"value","name":"chip_select","value":"None"}\r\n'
    b'{"type":"report_end","report":"plan"}\r\n'
)

PROGRAM_LO1_RESPONSE = (
    b'{"type":"report_begin","report":"program_lo1"}\r\n'
    b'{"type":"value","name":"lo1_frequency_mhz","value":3630.000}\r\n'
    b'{"type":"check","name":"lo1_frequency_mhz","expected":3630.000,"actual":3630.000,"result":"PASS"}\r\n'
    b'{"type":"value","name":"lo1_m","value":4095}\r\n'
    b'{"type":"value","name":"lo1_f","value":0}\r\n'
    b'{"type":"value","name":"lo1_n","value":55}\r\n'
    b'{"type":"value","name":"lo1_diva","value":1}\r\n'
    b'{"type":"report_end","report":"program_lo1"}\r\n'
)

PROGRAM_LO2_RESPONSE = (
    b'{"type":"report_begin","report":"program_lo2"}\r\n'
    b'{"type":"value","name":"lo2_frequency_mhz","value":3935.000}\r\n'
    b'{"type":"check","name":"lo2_frequency_mhz","expected":3935.000,"actual":3935.000,"result":"PASS"}\r\n'
    b'{"type":"value","name":"lo2_m","value":2603}\r\n'
    b'{"type":"value","name":"lo2_f","value":1617}\r\n'
    b'{"type":"value","name":"lo2_n","value":59}\r\n'
    b'{"type":"value","name":"lo2_diva","value":1}\r\n'
    b'{"type":"report_end","report":"program_lo2"}\r\n'
)

CHIP_LO1_RESPONSE = b"Chip select set to LO1\r\n"
CHIP_LO2_RESPONSE = b"Chip select set to LO2\r\n"
LOFREQ_OFF_TARGET_RESPONSE = b"LO frequency set.\r\n"
CHIP_OFF_RESPONSE = b"All chip selects deasserted.\r\n"


def rigol_responses():
    return [
        b"saTech WN2A ready",
        REFCHECK_RESPONSE,
        b"Reference clock set to REF1.\r\n",
        PINCHECK_RESPONSE,
        b"All chip selects deasserted.\r\n",
        ATTEN_RESPONSE,
        CHIP_LO1_RESPONSE,
        LOFREQ_OFF_TARGET_RESPONSE,
        CHIP_LO2_RESPONSE,
        LOFREQ_OFF_TARGET_RESPONSE,
        CHIP_OFF_RESPONSE,
        PLAN_RESPONSE,
        PROGRAM_LO1_RESPONSE,
        PROGRAM_LO2_RESPONSE,
    ]


def rigol_meter_responses():
    return rigol_responses() + [CHIP_OFF_RESPONSE]


class FakeSerial:
    instances = []
    events = None

    def __init__(self, port, baud, timeout):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.responses = list(FakeSerial.responses)
        self.response = b""
        self.writes = []
        self.reset_called = False
        self.flush_called = False
        FakeSerial.instances.append(self)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    @property
    def in_waiting(self):
        return len(self.response)

    def reset_input_buffer(self):
        self.reset_called = True

    def write(self, data):
        self.writes.append(data)
        if FakeSerial.events is not None:
            FakeSerial.events.append(f"serial:{data.decode('ascii').strip()}")
        self.response = self.responses.pop(0)

    def flush(self):
        self.flush_called = True

    def read(self, size):
        chunk = self.response[:size]
        self.response = self.response[size:]
        return chunk


LO1_EXPECTED = {
    "writes": [
        {"address": 0, "value": 0xAAA00000, "hex": "0xAAA00000"},
    ],
    "registers": {
        5: "0xAAA00005",
        4: "0xAAA00004",
        3: "0xAAA00003",
        2: "0xAAA00002",
        1: "0xAAA00001",
        0: "0xAAA00000",
    },
}

LO2_EXPECTED = {
    "writes": [
        {"address": 1, "value": 0xBBB00001, "hex": "0xBBB00001"},
        {"address": 0, "value": 0xBBB00000, "hex": "0xBBB00000"},
    ],
    "registers": {
        5: "0xBBB00005",
        4: "0xBBB00004",
        3: "0xBBB00003",
        2: "0xBBB00002",
        1: "0xBBB00001",
        0: "0xBBB00000",
    },
}


def fake_expected_registers(frequency_mhz):
    if frequency_mhz == 3630.0:
        return LO1_EXPECTED
    if frequency_mhz == 3935.0:
        return LO2_EXPECTED
    raise AssertionError(f"unexpected frequency {frequency_mhz}")


def decoded_from_expected(expected):
    return {"decoded_words": list(expected["writes"])}


class FakeRigol:
    def __init__(self, decodes, events):
        self.decodes = list(decodes)
        self.events = events
        self.decode_calls = []

    def scope_setup(self):
        self.events.append("rigol:setup")

    def start_new_waveform(self):
        self.events.append("rigol:new_waveform")

    def capture_waveform(self):
        self.events.append("rigol:capture")
        return {"capture": len(self.decode_calls)}

    def spi_decode(self, capture, expected_writes, expected_addresses):
        self.events.append("rigol:decode")
        self.decode_calls.append({
            "capture": capture,
            "expected_writes": expected_writes,
            "expected_addresses": expected_addresses,
        })
        return self.decodes.pop(0)


class FakeMeter:
    def __init__(self, readings, events=None):
        self.readings = list(readings)
        self.events = events
        self.calls = 0

    def read_measurement(self):
        self.calls += 1
        if self.events is not None:
            self.events.append("meter:read")
        return self.readings.pop(0)


def meter_reading(value, unit="mV"):
    return {
        "measurement": {
            "value": value,
            "unit": unit,
        }
    }


def volts_to_dbm(voltage):
    x = voltage
    return (((((((-9.460927 * x + 110.57352) * x - 538.8610489) * x + 1423.9059205) * x - 2219.08322) * x + 2073.3123) * x - 1122.5121) * x + 355.7665) * x - 112.663


class RunFullTestCase(unittest.TestCase):
    def setUp(self):
        FakeSerial.instances = []
        FakeSerial.events = None
        FakeSerial.responses = [
            b"saTech WN2A ready",
            REFCHECK_RESPONSE,
            b"Reference clock set to REF1.\r\n",
            PINCHECK_RESPONSE,
            b"All chip selects deasserted.\r\n",
            ATTEN_RESPONSE,
            PLAN_RESPONSE,
            PROGRAM_LO1_RESPONSE,
            PROGRAM_LO2_RESPONSE,
        ]

    def test_queries_unit_id_and_reports_pass(self):
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(config, serial_factory=FakeSerial)

        fake = FakeSerial.instances[0]
        self.assertEqual(fake.port, "/dev/fake")
        self.assertEqual(fake.baud, 115200)
        self.assertEqual(fake.timeout, 0.05)
        self.assertTrue(fake.reset_called)
        self.assertEqual(
            fake.writes,
            [
                b"id\n",
                b"fulltest refcheck\n",
                b"set ref1\n",
                b"fulltest pincheck\n",
                b"chip off\n",
                b"fulltest atten 12.00\n",
                b"fulltest plan 10.000\n",
                b"fulltest program lo1\n",
                b"fulltest program lo2\n",
            ],
        )
        self.assertTrue(fake.flush_called)
        self.assertTrue(report.passed)
        self.assertEqual(report.unit_id, "saTech WN2A ready")
        self.assertEqual(
            report.to_dict()["commands"],
            [
                "id",
                "fulltest refcheck",
                "set ref1",
                "fulltest pincheck",
                "chip off",
                "fulltest atten 12.00",
                "fulltest plan 10.000",
                "fulltest program lo1",
                "fulltest program lo2",
            ],
        )
        self.assertIn('"report":"refcheck"', report.steps[1].response)
        self.assertEqual(report.steps[2].response, "Reference clock set to REF1.")
        self.assertIn('"report":"pincheck"', report.steps[3].response)
        self.assertEqual(report.steps[4].response, "All chip selects deasserted.")
        self.assertIn('"report":"atten"', report.steps[5].response)
        self.assertIn('"report":"plan"', report.steps[6].response)
        self.assertIn('"report":"program_lo1"', report.steps[7].response)
        self.assertIn('"report":"program_lo2"', report.steps[8].response)
        self.assertEqual(report.values[0].name, "atten_db")
        self.assertEqual(report.values[0].value, 12.0)
        self.assertEqual(
            [check.name for check in report.checks],
            [
                "unit_id",
                "ref1_selected",
                "ref2_selected",
                "refs_off",
                "pin_checks",
                "lo1_frequency_mhz",
                "lo2_frequency_mhz",
                "lo1_frequency_mhz",
                "lo2_frequency_mhz",
            ],
        )

    def test_reports_failed_id_check(self):
        FakeSerial.responses = [
            b"unexpected unit",
            REFCHECK_RESPONSE,
            b"Reference clock set to REF1.\r\n",
            PINCHECK_RESPONSE,
            b"All chip selects deasserted.\r\n",
            ATTEN_RESPONSE,
            PLAN_RESPONSE,
            PROGRAM_LO1_RESPONSE,
            PROGRAM_LO2_RESPONSE,
        ]
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(config, serial_factory=FakeSerial)

        fake = FakeSerial.instances[0]
        self.assertEqual(fake.writes, [b"id\n"])
        self.assertFalse(report.passed)
        self.assertEqual(report.unit_id, "unexpected unit")
        self.assertEqual([step.name for step in report.steps], ["unit_id"])
        self.assertEqual(report.checks[0].name, "unit_id")
        self.assertEqual(report.checks[0].expected, "saTech WN2A ready")
        self.assertEqual(report.checks[0].actual, "unexpected unit")
        self.assertFalse(report.checks[0].passed)

    def test_report_serializes_to_dict(self):
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(config, serial_factory=FakeSerial)

        self.assertEqual(
            report.to_dict(),
            {
                "passed": True,
                "unit_id": "saTech WN2A ready",
                "commands": [
                    "id",
                    "fulltest refcheck",
                    "set ref1",
                    "fulltest pincheck",
                    "chip off",
                    "fulltest atten 12.00",
                    "fulltest plan 10.000",
                    "fulltest program lo1",
                    "fulltest program lo2",
                ],
                "checks": [
                    {
                        "name": "unit_id",
                        "expected": "saTech WN2A ready",
                        "actual": "saTech WN2A ready",
                        "passed": True,
                    },
                    {
                        "name": "ref1_selected",
                        "expected": "Ref1 on : Ref2 off",
                        "actual": "Ref1 on : Ref2 off",
                        "passed": True,
                    },
                    {
                        "name": "ref2_selected",
                        "expected": "Ref1 off : Ref2 on",
                        "actual": "Ref1 off : Ref2 on",
                        "passed": True,
                    },
                    {
                        "name": "refs_off",
                        "expected": "Ref1 off : Ref2 off",
                        "actual": "Ref1 off : Ref2 off",
                        "passed": True,
                    },
                    {
                        "name": "pin_checks",
                        "expected": "PASS",
                        "actual": "PASS",
                        "passed": True,
                    },
                    {
                        "name": "lo1_frequency_mhz",
                        "expected": 3630.0,
                        "actual": 3630.0,
                        "passed": True,
                    },
                    {
                        "name": "lo2_frequency_mhz",
                        "expected": 3935.0,
                        "actual": 3935.0,
                        "passed": True,
                    },
                    {
                        "name": "lo1_frequency_mhz",
                        "expected": 3630.0,
                        "actual": 3630.0,
                        "passed": True,
                    },
                    {
                        "name": "lo2_frequency_mhz",
                        "expected": 3935.0,
                        "actual": 3935.0,
                        "passed": True,
                    },
                ],
                "values": [
                    {
                        "name": "atten_db",
                        "value": 12.0,
                    },
                    {
                        "name": "rfin_mhz",
                        "value": 10.0,
                    },
                    {
                        "name": "if1_mhz",
                        "value": 3620.0,
                    },
                    {
                        "name": "if2_mhz",
                        "value": 315.0,
                    },
                    {
                        "name": "lo1_frequency_mhz",
                        "value": 3630.0,
                    },
                    {
                        "name": "lo2_frequency_mhz",
                        "value": 3935.0,
                    },
                    {
                        "name": "atten_db",
                        "value": 12.0,
                    },
                    {
                        "name": "chip_select",
                        "value": "None",
                    },
                    {
                        "name": "lo1_frequency_mhz",
                        "value": 3630.0,
                    },
                    {
                        "name": "lo1_m",
                        "value": 4095,
                    },
                    {
                        "name": "lo1_f",
                        "value": 0,
                    },
                    {
                        "name": "lo1_n",
                        "value": 55,
                    },
                    {
                        "name": "lo1_diva",
                        "value": 1,
                    },
                    {
                        "name": "lo2_frequency_mhz",
                        "value": 3935.0,
                    },
                    {
                        "name": "lo2_m",
                        "value": 2603,
                    },
                    {
                        "name": "lo2_f",
                        "value": 1617,
                    },
                    {
                        "name": "lo2_n",
                        "value": 59,
                    },
                    {
                        "name": "lo2_diva",
                        "value": 1,
                    },
                ],
            },
        )

    def test_rigol_register_verification_sequences_scope_and_passes(self):
        events = []
        FakeSerial.events = events
        FakeSerial.responses = rigol_responses()
        rigol = FakeRigol(
            [
                decoded_from_expected(LO1_EXPECTED),
                decoded_from_expected(LO2_EXPECTED),
            ],
            events,
        )
        provider_calls = []

        def expected_register_provider(lo_name, frequency_mhz, previous_frequency_mhz):
            provider_calls.append((lo_name, previous_frequency_mhz, frequency_mhz))
            return fake_expected_registers(frequency_mhz)

        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(
            config,
            serial_factory=FakeSerial,
            rigol=rigol,
            expected_register_provider=expected_register_provider,
        )

        self.assertTrue(report.passed)
        self.assertEqual(
            events,
            [
                "serial:id",
                "serial:fulltest refcheck",
                "serial:set ref1",
                "serial:fulltest pincheck",
                "serial:chip off",
                "serial:fulltest atten 12.00",
                "rigol:setup",
                "serial:chip lo1",
                "serial:lofreq 123.345",
                "serial:chip lo2",
                "serial:lofreq 123.345",
                "serial:chip off",
                "serial:fulltest plan 10.000",
                "rigol:new_waveform",
                "serial:fulltest program lo1",
                "rigol:capture",
                "rigol:decode",
                "rigol:new_waveform",
                "serial:fulltest program lo2",
                "rigol:capture",
                "rigol:decode",
            ],
        )
        self.assertEqual(len(report.registers), 12)
        self.assertEqual(
            provider_calls,
            [
                ("lo1", 123.345, 3630.0),
                ("lo2", 123.345, 3935.0),
            ],
        )
        self.assertEqual(rigol.decode_calls[0]["expected_addresses"], [0])
        self.assertEqual(rigol.decode_calls[1]["expected_addresses"], [1, 0])
        checks = {check.name: check for check in report.checks}
        self.assertTrue(checks["lo1_register_verification"].passed)
        self.assertTrue(checks["lo2_register_verification"].passed)
        self.assertIn("registers", report.to_dict())

    def test_rigol_register_verification_stops_after_lo1_failure(self):
        events = []
        FakeSerial.events = events
        FakeSerial.responses = rigol_responses()
        lo1_bad = {
            "decoded_words": [
                {"address": 0, "value": 0xBAD00000, "hex": "0xBAD00000"},
            ]
        }
        rigol = FakeRigol([lo1_bad], events)
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(
            config,
            serial_factory=FakeSerial,
            rigol=rigol,
            expected_register_provider=fake_expected_registers,
        )

        self.assertFalse(report.passed)
        self.assertNotIn("serial:fulltest program lo2", events)
        self.assertEqual(report.to_dict()["commands"][-1], "fulltest program lo1")
        checks = {check.name: check for check in report.checks}
        self.assertFalse(checks["lo1_r0"].passed)
        self.assertFalse(checks["lo1_register_verification"].passed)

    def test_rigol_register_verification_reports_lo2_failure_after_lo1_passes(self):
        events = []
        FakeSerial.events = events
        FakeSerial.responses = rigol_responses()
        lo2_bad = {
            "decoded_words": [
                {"address": 1, "value": 0xBAD00001, "hex": "0xBAD00001"},
                {"address": 0, "value": 0xBBB00000, "hex": "0xBBB00000"},
            ]
        }
        rigol = FakeRigol(
            [
                decoded_from_expected(LO1_EXPECTED),
                lo2_bad,
            ],
            events,
        )
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(
            config,
            serial_factory=FakeSerial,
            rigol=rigol,
            expected_register_provider=fake_expected_registers,
        )

        self.assertFalse(report.passed)
        self.assertIn("serial:fulltest program lo2", events)
        checks = {check.name: check for check in report.checks}
        self.assertTrue(checks["lo1_register_verification"].passed)
        self.assertFalse(checks["lo2_r1"].passed)
        self.assertFalse(checks["lo2_register_verification"].passed)

    def test_rigol_and_meter_report_final_measurement_from_third_read(self):
        events = []
        FakeSerial.events = events
        FakeSerial.responses = rigol_meter_responses()
        rigol = FakeRigol(
            [
                decoded_from_expected(LO1_EXPECTED),
                decoded_from_expected(LO2_EXPECTED),
            ],
            events,
        )
        meter = FakeMeter(
            [
                meter_reading(1.1),
                meter_reading(1.2),
                meter_reading(1.3),
            ],
            events,
        )
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(
            config,
            serial_factory=FakeSerial,
            rigol=rigol,
            meter=meter,
            expected_register_provider=fake_expected_registers,
        )

        self.assertTrue(report.passed)
        self.assertEqual(meter.calls, 3)
        self.assertEqual(
            events[-5:],
            [
                "rigol:decode",
                "serial:chip off",
                "meter:read",
                "meter:read",
                "meter:read",
            ],
        )
        values = {value.name: value.value for value in report.values}
        self.assertEqual(values["bk390a_voltage_v"], 0.0013)
        self.assertEqual(values["logamp_power_dbm"], volts_to_dbm(0.0013))
        self.assertEqual(report.to_dict()["commands"][-1], "chip off")

    def test_meter_does_not_run_after_lo2_failure(self):
        events = []
        FakeSerial.events = events
        FakeSerial.responses = rigol_responses()
        lo2_bad = {
            "decoded_words": [
                {"address": 1, "value": 0xBAD00001, "hex": "0xBAD00001"},
                {"address": 0, "value": 0xBBB00000, "hex": "0xBBB00000"},
            ]
        }
        rigol = FakeRigol(
            [
                decoded_from_expected(LO1_EXPECTED),
                lo2_bad,
            ],
            events,
        )
        meter = FakeMeter([meter_reading(1.3)], events)
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(
            config,
            serial_factory=FakeSerial,
            rigol=rigol,
            meter=meter,
            expected_register_provider=fake_expected_registers,
        )

        self.assertFalse(report.passed)
        self.assertEqual(meter.calls, 0)
        self.assertNotIn("meter:read", events)


class RigolFullTestAdapterCase(unittest.TestCase):
    def test_scope_setup_sends_verified_scpi_sequence(self):
        writes = []

        class CaptureRigol(RigolFullTestAdapter):
            def _write(self, scpi):
                writes.append(scpi)

        rigol = CaptureRigol()

        rigol.scope_setup()

        self.assertEqual(
            writes,
            [
                ":STOP",
                ":CHAN1:DISP ON",
                ":CHAN2:DISP ON",
                ":CHAN1:SCALe 2.0",
                ":TRIGger:MODE EDGE",
                ":TRIGger:EDGE:SOURce CHAN1",
                ":TRIGger:EDGE:LEVel 1.28",
                ":TRIGger:EDGE:SWEep SING",
                ":WAVeform:POINts:MODE RAW",
                ":TIMebase:SCALe 5.0us",
            ],
        )


if __name__ == "__main__":
    unittest.main()
