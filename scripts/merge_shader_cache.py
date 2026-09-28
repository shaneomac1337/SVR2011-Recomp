"""Merge ReXGlue Vulkan shader caches from several play sessions into one.

The result ships in the player package so a first launch builds known pipelines at
startup instead of compiling them mid-game. Caches hold translated game shaders, so the
output stays local and out of Git, like svr2011.exe.
"""
import argparse
import struct
from pathlib import Path

TITLE_ID = "5451085D"
SHADER_MAGIC = b"XESH"
PIPELINE_MAGIC = b"XEPS"
SHADER_HEADER = 12  # ucode hash (8) + dword count and type (4)
PIPELINE_RECORD = 68  # description hash (8) + pipeline description (60)


def read_shaders(path):
    """Return (file header, records); stops at a truncated tail like the runtime does."""
    data = path.read_bytes()
    if len(data) < 8 or data[:4] != SHADER_MAGIC:
        raise ValueError(f"{path} is not a shader storage file")
    records, offset = [], 8
    while offset + SHADER_HEADER <= len(data):
        (packed,) = struct.unpack_from("<I", data, offset + 8)
        end = offset + SHADER_HEADER + (packed & 0x7FFFFFFF) * 4
        if end > len(data):
            break
        records.append(data[offset:end])
        offset = end
    return data[:8], records


def read_pipelines(path):
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != PIPELINE_MAGIC:
        raise ValueError(f"{path} is not a pipeline storage file")
    count = (len(data) - 12) // PIPELINE_RECORD
    return data[:12], [data[12 + i * PIPELINE_RECORD:12 + (i + 1) * PIPELINE_RECORD] for i in range(count)]


def merge(kind, reader, sources, output):
    header, merged, seen = None, [], set()
    for source in sources:
        if not source.exists():
            continue
        source_header, records = reader(source)
        if header is None:
            header = source_header
        elif source_header != header:
            # A different translator or render path: its records are unusable here.
            print(f"skipped {source}: {kind} header differs")
            continue
        for record in records:
            if record not in seen:
                seen.add(record)
                merged.append(record)
    if header is None:
        return 0
    output.write_bytes(header + b"".join(merged))
    return len(merged)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Folder for the merged shareable cache")
    parser.add_argument("sources", type=Path, nargs="+", help="shaders/shareable folders to merge")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    shaders = merge("shader", read_shaders, [s / f"{TITLE_ID}.xsh" for s in args.sources],
                    args.output / f"{TITLE_ID}.xsh")
    pipelines = merge("pipeline", read_pipelines, [s / f"{TITLE_ID}.fsi.vk.xpso" for s in args.sources],
                      args.output / f"{TITLE_ID}.fsi.vk.xpso")
    print(f"{shaders} shaders, {pipelines} FSI pipelines -> {args.output}")


if __name__ == "__main__":
    main()
