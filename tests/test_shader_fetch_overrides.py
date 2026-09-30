import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

from shader_fetch_overrides import overrides, texture_fetches  # noqa: E402


def container(fetch_words):
    """A minimal container: header, shader record, then one exec + one fetch."""
    header = bytearray(0x24)
    struct.pack_into(">III", header, 0, 0x102A1100, 0x40, 24)
    struct.pack_into(">I", header, 0x18, 0x24)
    shader = struct.pack(">II", 0, 24) + bytes(0x40 - 0x24 - 8)
    # CF pair: exec (opcode 2 = ExecEnd) at address 1, count 1, sequence 1 (fetch).
    cf = 1 | 1 << 12 | 1 << 16 | 2 << 44
    code = struct.pack(">III", cf & 0xFFFFFFFF, cf >> 32, 0)
    code += struct.pack(">III", *fetch_words)
    return bytes(header) + shader + code


class FetchOverrideTest(unittest.TestCase):
    def test_fetch_constant_filters_are_not_overrides(self):
        d1 = 3 << 12 | 3 << 14 | 3 << 16 | 7 << 18
        fetches = list(texture_fetches(container((1 | 5 << 20, d1, 0))))
        self.assertEqual(len(fetches), 1)
        slot, fetch = fetches[0]
        self.assertEqual(slot, 1)
        self.assertEqual(fetch["const"], 5)
        self.assertEqual(overrides(fetch), [])

    def test_linear_filter_and_lod_bias_are_reported(self):
        d1 = 1 << 12 | 3 << 14 | 3 << 16 | 7 << 18
        d2 = (0x7F & -8) << 2  # lod bias -0.5 (in 1/16 steps)
        _, fetch = next(texture_fetches(container((1, d1, d2))))
        self.assertEqual(overrides(fetch), ["mag=linear", "lod_bias=-0.5"])


if __name__ == "__main__":
    unittest.main()
