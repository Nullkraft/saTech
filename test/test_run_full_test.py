import unittest

from run_full_test import FullTestConfig, run_full_test


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
        self.assertEqual(fake.writes, [b"id\n", b"set ref1\n", b"chip off\n"])
        self.assertTrue(fake.flush_called)
        self.assertTrue(report.passed)
        self.assertEqual(report.unit_id, "saTech WN2A ready")
        self.assertEqual([step.name for step in report.steps], ["unit_id", "set_ref1", "chip_off"])
        self.assertEqual(report.steps[1].response, "Reference clock set to REF1.")
        self.assertEqual(report.steps[2].response, "All chip selects deasserted.")
        self.assertEqual(
            report.raw_response_hex,
            "73 61 54 65 63 68 20 57 4E 32 41 20 72 65 61 64 79",
        )

    def test_reports_failed_id_check(self):
        FakeSerial.responses = [
            b"unexpected unit",
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
                "steps": [
                    {
                        "name": "unit_id",
                        "command": "id",
                        "response": "saTech WN2A ready",
                        "response_hex": "73 61 54 65 63 68 20 57 4E 32 41 20 72 65 61 64 79",
                    },
                    {
                        "name": "set_ref1",
                        "command": "set ref1",
                        "response": "Reference clock set to REF1.",
                        "response_hex": "52 65 66 65 72 65 6E 63 65 20 63 6C 6F 63 6B 20 73 65 74 20 74 6F 20 52 45 46 31 2E 0D 0A",
                    },
                    {
                        "name": "chip_off",
                        "command": "chip off",
                        "response": "All chip selects deasserted.",
                        "response_hex": "41 6C 6C 20 63 68 69 70 20 73 65 6C 65 63 74 73 20 64 65 61 73 73 65 72 74 65 64 2E 0D 0A",
                    },
                ],
                "checks": [
                    {
                        "name": "unit_id",
                        "expected": "saTech WN2A ready",
                        "actual": "saTech WN2A ready",
                        "passed": True,
                    }
                ],
                "raw_response_hex": "73 61 54 65 63 68 20 57 4E 32 41 20 72 65 61 64 79",
            },
        )


if __name__ == "__main__":
    unittest.main()
