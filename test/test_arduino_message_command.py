import unittest

import serial_command_test


class ArduinoMessageCommandTestCase(unittest.TestCase):
    def test_sends_binary_query(self):
        command = serial_command_test.COMMANDS["arduino-message"]

        self.assertEqual(command["tx"], bytes.fromhex("FF 17 00 00"))
        self.assertEqual(command["expect"], "saTech WN2A ready")


if __name__ == "__main__":
    unittest.main()
