import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import merge_shader_cache as cache  # noqa: E402

SHADER_FILE_HEADER = b"XESH" + bytes.fromhex("20201219")
PIPELINE_FILE_HEADER = b"XEPS" + bytes.fromhex("0100000020260228")


def shader(hash_value, dwords):
    return struct.pack("<QI", hash_value, len(dwords)) + b"".join(struct.pack("<I", d) for d in dwords)


def pipeline(tag):
    return bytes([tag]) * cache.PIPELINE_RECORD


class MergeShaderCacheTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())

    def write(self, folder, shaders=b"", pipelines=b"", pipeline_header=PIPELINE_FILE_HEADER):
        path = self.root / folder
        path.mkdir()
        (path / "5451085D.xsh").write_bytes(SHADER_FILE_HEADER + shaders)
        (path / "5451085D.fsi.vk.xpso").write_bytes(pipeline_header + pipelines)
        return path

    def test_merges_and_removes_duplicates(self):
        a = self.write("a", shader(1, [7, 8]) + shader(2, [9]), pipeline(1) + pipeline(2))
        b = self.write("b", shader(2, [9]) + shader(3, []), pipeline(2) + pipeline(3))
        out = self.root / "out"
        out.mkdir()
        self.assertEqual(cache.merge("shader", cache.read_shaders, [a / "5451085D.xsh", b / "5451085D.xsh"],
                                     out / "5451085D.xsh"), 3)
        self.assertEqual(cache.merge("pipeline", cache.read_pipelines,
                                     [a / "5451085D.fsi.vk.xpso", b / "5451085D.fsi.vk.xpso"],
                                     out / "5451085D.fsi.vk.xpso"), 3)
        header, records = cache.read_shaders(out / "5451085D.xsh")
        self.assertEqual(header, SHADER_FILE_HEADER)
        self.assertEqual(records, [shader(1, [7, 8]), shader(2, [9]), shader(3, [])])

    def test_ignores_truncated_tail(self):
        a = self.write("a", shader(1, [5]) + shader(2, [6, 7])[:-3])
        _, records = cache.read_shaders(a / "5451085D.xsh")
        self.assertEqual(records, [shader(1, [5])])

    def test_skips_sources_with_another_version(self):
        a = self.write("a", pipelines=pipeline(1))
        b = self.write("b", pipelines=pipeline(2), pipeline_header=b"XEPS" + bytes.fromhex("0100000020250101"))
        out = self.root / "merged.xpso"
        self.assertEqual(cache.merge("pipeline", cache.read_pipelines,
                                     [a / "5451085D.fsi.vk.xpso", b / "5451085D.fsi.vk.xpso"], out), 1)


if __name__ == "__main__":
    unittest.main()
