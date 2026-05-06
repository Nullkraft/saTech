import unittest

import serial_command_test


class AsciiCommandTestCase(unittest.TestCase):
    def test_ascii_message_command(self):
        print("test_ascii_commands: ascii-message sends 17FF")
        command = serial_command_test.COMMANDS["ascii-message"]

        self.assertEqual(command["tx"], b"17FF\n")
        self.assertEqual(command["expect"], "saTech WN2A ready")

    def test_ascii_ref_off_command(self):
        print("test_ascii_commands: ascii-ref-off sends 04FF")
        command = serial_command_test.COMMANDS["ascii-ref-off"]

        self.assertEqual(command["tx"], b"04FF\n")

    def test_ascii_ref1_on_command(self):
        print("test_ascii_commands: ascii-select-ref1 sends 0CFF")
        command = serial_command_test.COMMANDS["ascii-select-ref1"]

        self.assertEqual(command["tx"], b"0CFF\n")

    def test_ascii_ref2_on_command(self):
        print("test_ascii_commands: ascii-select-ref2 sends 14FF")
        command = serial_command_test.COMMANDS["ascii-select-ref2"]

        self.assertEqual(command["tx"], b"14FF\n")


if __name__ == "__main__":
    unittest.main()
