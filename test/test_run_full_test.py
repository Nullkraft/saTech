import unittest

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


class FakeSerial:
    instances = []

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
        self.response = self.responses.pop(0)

    def flush(self):
        self.flush_called = True

    def read(self, size):
        chunk = self.response[:size]
        self.response = self.response[size:]
        return chunk


class RunFullTestCase(unittest.TestCase):
    def setUp(self):
        FakeSerial.instances = []
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


if __name__ == "__main__":
    unittest.main()
