import struct
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from scripts.inspect_disc import Disc, MAGIC, SECTOR, inspect_xex, main


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def disc(self, name=b"default.xex", pointer=0, length=4):
        data = bytearray(40 * SECTOR)
        data[0x10000:0x10000 + 20] = MAGIC
        data[0x10800 - 20:0x10800] = MAGIC
        struct.pack_into("<II", data, 0x10000 + 20, 34, SECTOR)
        struct.pack_into("<HHIIBB", data, 34 * SECTOR, pointer, 0, 36, length, 0, len(name))
        data[34 * SECTOR + 14:34 * SECTOR + 14 + len(name)] = name
        data[36 * SECTOR:36 * SECTOR + 4] = b"test"
        path = self.root / "disc.iso"
        path.write_bytes(data)
        disc = Disc(path)
        self.addCleanup(disc.close)
        return disc

    def test_inventory_extract_and_verify_existing_file(self):
        disc = self.disc()
        entry, = disc.files()
        assets = self.root / "assets"
        checksum = disc.extract(entry, assets)
        self.assertEqual((assets / "default.xex").read_bytes(), b"test")
        self.assertEqual(disc.extract(entry, assets), checksum)
        (assets / "default.xex").write_bytes(b"other")
        with self.assertRaisesRegex(ValueError, "differs"):
            disc.extract(entry, assets)
        self.assertEqual((assets / "default.xex").read_bytes(), b"other")

    def test_invalid_name_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            self.disc(b"../escape").files()

    def test_directory_pointer_out_of_bounds_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "pointer"):
            self.disc(pointer=1000).files()

    def test_file_beyond_image_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "outside image"):
            self.disc(length=100 * SECTOR).files()

    def test_extraction_escape_is_rejected(self):
        disc = self.disc()
        with self.assertRaisesRegex(ValueError, "escapes"):
            disc.extract({"path": "../outside", "offset": 0, "size": 4}, self.root / "assets")

    def test_empty_image_is_rejected(self):
        path = self.root / "empty.iso"
        path.touch()
        with self.assertRaisesRegex(ValueError, "No supported"):
            Disc(path)

    def test_xex_header_requires_complete_table(self):
        data = bytearray(24)
        data[:4] = b"XEX2"
        struct.pack_into(">I", data, 20, 1)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            inspect_xex(data)

    def test_report_collision_does_not_modify_extracted_executable(self):
        disc = self.disc()
        entry, = disc.files()
        assets = self.root / "assets"
        disc.extract(entry, assets)
        executable = assets / "default.xex"
        with patch('sys.argv', ['inspect_disc', str(disc.path), '--extract-all',
                               str(assets), '--report', str(executable)]):
            with redirect_stderr(StringIO()), self.assertRaises(SystemExit) as error:
                main()
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(executable.read_bytes(), b"test")

    def test_report_collision_with_executable_only_is_rejected(self):
        disc = self.disc()
        executable = self.root / "default.xex"
        executable.write_bytes(b"original")
        with patch('sys.argv', ['inspect_disc', str(disc.path), '--extract-executable',
                               str(executable), '--report', str(executable)]):
            with redirect_stderr(StringIO()), self.assertRaises(SystemExit) as error:
                main()
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(executable.read_bytes(), b"original")


if __name__ == "__main__":
    unittest.main()
