import unittest

from run_full_test import FullTestConfig, run_full_test


REFCHECK_RESPONSE = (
    b'{"type":"report_begin","report":"refcheck"}\r\n'
    b'{"type":"check","name":"ref1_selected","expected":"Ref1 on : Ref2 off","actual":"Ref1 on : Ref2 off","result":"PASS"}\r\n'
    b'{"type":"check","name":"ref2_selected","expected":"Ref1 off : Ref2 on","actual":"Ref1 off : Ref2 on","result":"PASS"}\r\n'
    b'{"type":"check","name":"refs_off","expected":"Ref1 off : Ref2 off","actual":"Ref1 off : Ref2 off","result":"PASS"}\r\n'
    b'{"type":"report_end","report":"refcheck"}\r\n'
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
            b"All chip selects deasserted.\r\n",
        ]

    def test_queries_unit_id_and_reports_pass(self):
        config = FullTestConfig(port="/dev/fake", timeout=0.01, open_delay=0.0)

        report = run_full_test(config, serial_factory=FakeSerial)

        fake = FakeSerial.instances[0]
        self.assertEqual(fake.port, "/dev/fake")
        self.assertEqual(fake.baud, 115200)
        self.assertEqual(fake.timeout, 0.05)
        self.assertTrue(fake.reset_called)
        self.assertEqual(fake.writes, [b"id\n", b"fulltest refcheck\n", b"set ref1\n", b"chip off\n"])
        self.assertTrue(fake.flush_called)
        self.assertTrue(report.passed)
        self.assertEqual(report.unit_id, "saTech WN2A ready")
        self.assertEqual(
            report.to_dict()["commands"],
            ["id", "fulltest refcheck", "set ref1", "chip off"],
        )
        self.assertIn('"report":"refcheck"', report.steps[1].response)
        self.assertEqual(report.steps[2].response, "Reference clock set to REF1.")
        self.assertEqual(report.steps[3].response, "All chip selects deasserted.")
        self.assertEqual(
            [check.name for check in report.checks],
            ["unit_id", "ref1_selected", "ref2_selected", "refs_off"],
        )

    def test_reports_failed_id_check(self):
        FakeSerial.responses = [
            b"unexpected unit",
            REFCHECK_RESPONSE,
            b"Reference clock set to REF1.\r\n",
            b"All chip selects deasserted.\r\n",
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
                "commands": ["id", "fulltest refcheck", "set ref1", "chip off"],
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
                    }
                ],
            },
        )


if __name__ == "__main__":
    unittest.main()
