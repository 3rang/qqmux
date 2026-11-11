# SPDX-License-Identifier: Apache-2.0

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())


class RegisterAccessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="qqmem-test-", dir=Path.cwd())
        self.addCleanup(self.temp.cleanup)
        self.device = Path(self.temp.name) / "memory.bin"
        self.page_size = os.sysconf("SC_PAGE_SIZE")
        self.original = bytes((i * 37 + 11) % 256 for i in range(3 * self.page_size))
        self.device.write_bytes(self.original)

    def run_cli(self, *args, device=None):
        return subprocess.run(
            [BINARY, *map(str, args), "--device", str(device or self.device)],
            capture_output=True, text=True, timeout=5,
        )

    def assert_success(self, result, stdout):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, stdout)

    def test_reads_have_exact_width_and_do_not_change_memory(self):
        for width in (8, 16, 32):
            size = width // 8
            for offset in (0, 16, self.page_size - size, self.page_size,
                           2 * self.page_size + 8):
                with self.subTest(width=width, offset=offset):
                    value = int.from_bytes(self.original[offset:offset + size], sys.byteorder)
                    result = self.run_cli("read", hex(offset), "--width", width)
                    self.assert_success(result, f"0x{value:0{width // 4}X}\n")
        self.assertEqual(self.device.read_bytes(), self.original)

    def test_writes_change_only_the_requested_bytes(self):
        for width in (8, 16, 32):
            size = width // 8
            for offset in (0, self.page_size - size, self.page_size,
                           2 * self.page_size + 8):
                for value in (0, (1 << width) - 1, 0xA5):
                    with self.subTest(width=width, offset=offset, value=value):
                        self.device.write_bytes(self.original)
                        result = self.run_cli("write", hex(offset), hex(value), "--width", width)
                        self.assert_success(result, "")
                        expected = bytearray(self.original)
                        expected[offset:offset + size] = value.to_bytes(size, sys.byteorder)
                        self.assertEqual(self.device.read_bytes(), expected)

    def test_decimal_hex_and_octal_operands(self):
        for address, value in (("16", "165"), ("0x10", "0xA5"), ("020", "0245")):
            with self.subTest(address=address, value=value):
                self.assert_success(self.run_cli("write", address, value, "--width", 8), "")
                self.assertEqual(self.device.read_bytes()[16], 165)

    def test_options_do_not_depend_on_posixly_correct(self):
        env = dict(os.environ, POSIXLY_CORRECT="1")
        result = subprocess.run(
            [BINARY, "read", "0", "--width", "8", "--device", str(self.device)],
            capture_output=True, text=True, timeout=5, env=env,
        )
        self.assert_success(result, "0x0B\n")

    def test_invalid_arguments_are_rejected_before_opening_device(self):
        cases = [
            (), ("erase", "0", "--width", "32"),
            ("read",), ("write", "0"),
            ("read", "0"), ("read", "0", "--width"),
            ("read", "0", "--width", "64"),
            ("read", "0", "--width", "0"),
            ("read", "0", "--width", "32", "--width", "32"),
            ("read", "0", "--width", "32", "--unknown"),
            ("read", "0", "--width", "32", "extra"),
            ("write", "0", "0x100", "--width", "8"),
            ("write", "0", "0x10000", "--width", "16"),
            ("write", "0", "0x100000000", "--width", "32"),
            ("read", "1", "--width", "16"),
            ("read", "2", "--width", "32"),
            ("read", "0x8000000000000000", "--width", "8"),
        ]
        for operand in ("", "-1", "+1", " 1", "1 ", "0x", "08", "12oops",
                        "0x10000000000000000"):
            cases.extend([
                ("read", operand, "--width", "8"),
                ("write", "0", operand, "--width", "8"),
            ])
        for args in cases:
            with self.subTest(args=args):
                result = self.run_cli(*args, device=Path(self.temp.name) / "absent")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertTrue(result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("cannot open", result.stderr)
        self.assertEqual(self.device.read_bytes(), self.original)

    def test_duplicate_or_empty_device_is_rejected(self):
        for args in (("--device", str(self.device)), ("--device", "")):
            with self.subTest(args=args):
                result = self.run_cli("read", "0", "--width", "8", *args)
                self.assertEqual(result.returncode, 2)
                self.assertIn("specify --device once", result.stderr)

    def test_missing_device_is_not_created(self):
        missing = Path(self.temp.name) / "absent"
        for command in (("read", "0"), ("write", "0", "1")):
            with self.subTest(command=command):
                result = self.run_cli(*command, "--width", "8", device=missing)
                self.assertEqual(result.returncode, 1)
                self.assertIn("cannot open", result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertFalse(missing.exists())

    def test_unmappable_device_is_reported(self):
        result = self.run_cli("read", "0", "--width", "8", device=Path(self.temp.name))
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot map", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_access_past_file_end_is_rejected(self):
        for contents in (b"", b"\xAA", b"\xAA\xBB\xCC"):
            self.device.write_bytes(contents)
            for command in (("read", "0"), ("write", "0", "1")):
                with self.subTest(contents=contents, command=command):
                    result = self.run_cli(*command, "--width", "32")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("beyond the end", result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual(self.device.read_bytes(), contents)

    def test_last_aligned_access_in_file(self):
        for width in (8, 16, 32):
            with self.subTest(width=width):
                offset = len(self.original) - width // 8
                self.assert_success(self.run_cli("write", offset, "0", "--width", width), "")
                self.assert_success(self.run_cli("read", offset, "--width", width),
                                    f"0x{0:0{width // 4}X}\n")

    def test_output_failure_is_reported(self):
        with open("/dev/full", "w") as full:
            result = subprocess.run(
                [BINARY, "read", "0", "--width", "8", "--device", str(self.device)],
                stdout=full, stderr=subprocess.PIPE, text=True, timeout=5,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot write output", result.stderr)

    def test_help_never_opens_memory(self):
        for args in (("--help",), ("read", "--help"), ("write", "-h")):
            with self.subTest(args=args):
                result = subprocess.run([BINARY, *args], capture_output=True,
                                        text=True, timeout=5)
                self.assertEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stdout)
                self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
