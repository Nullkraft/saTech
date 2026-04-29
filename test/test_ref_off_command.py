import unittest

import serial_command_test


class RefOffCommandTestCase(unittest.TestCase):
    def test_sends_binary_control_word(self):
        print("test_ref_off_command: ref-off sends FF 04 00 00")
        command = serial_command_test.COMMANDS["ref-off"]

        self.assertEqual(command["tx"], bytes.fromhex("FF 04 00 00"))


if __name__ == "__main__":
    unittest.main()
