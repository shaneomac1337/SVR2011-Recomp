"""List texture fetches whose instruction overrides the fetch constant.

Xenos texture fetch instructions carry their own filters, LOD bias, sample
location and texel offsets; a filter of 3 means "use the fetch constant".
XenosRecomp ignores these fields, so any fetch that sets them samples
differently in the native renderer than in the emulated path (which honours
them). Reads the containers --svr_shader_dump_dir captured (game-derived,
local) and prints one line per overriding fetch plus a summary.
"""

from __future__ import annotations

import argparse
import collections
import struct
import sys
from pathlib import Path

EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}
FILTERS = {0: "point", 1: "linear", 2: "basemap", 3: "fetch-const"}


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def texture_fetches(container: bytes):
    """Yields (instruction slot, decoded fields) for each texture fetch."""
    virtual_size = be32(container, 4)
    shader_offset = be32(container, 0x18)
    physical_offset = be32(container, shader_offset)
    size = be32(container, shader_offset + 4)
    code = virtual_size + physical_offset
    words = [be32(container, code + i * 4) for i in range(size // 4)]
    instr_size = size
    address_bytes = 0
    cf_index = 0
    while address_bytes < instr_size and cf_index * 3 + 2 < len(words):
        w0, w1, w2 = words[cf_index * 3: cf_index * 3 + 3]
        pair = (w0 | (w1 & 0xFFFF) << 32, (w1 >> 16) | w2 << 16)
        for cf in pair:
            opcode = (cf >> 44) & 0xF
            if opcode not in EXEC_OPCODES:
                continue
            address = cf & 0xFFF
            count = (cf >> 12) & 0x7
            sequence = (cf >> 16) & 0xFFF
            instr_size = min(instr_size, address * 12) if address else instr_size
            for i in range(count):
                slot = address + i
                if (sequence >> (2 * i)) & 1 and slot * 3 + 2 < len(words):
                    d0, d1, d2 = words[slot * 3: slot * 3 + 3]
                    if d0 & 0x1F != 1:  # texture fetch
                        continue
                    yield slot, {
                        "const": (d0 >> 20) & 0x1F,
                        "denorm": (d0 >> 25) & 1,
                        "mag": (d1 >> 12) & 3,
                        "min": (d1 >> 14) & 3,
                        "mip": (d1 >> 16) & 3,
                        "aniso": (d1 >> 18) & 7,
                        "comp_lod": (d1 >> 28) & 1,
                        "reg_lod": (d1 >> 29) & 1,
                        "reg_gradients": d2 & 1,
                        "sample_location": (d2 >> 1) & 1,
                        "lod_bias": signed((d2 >> 2) & 0x7F, 7),
                        "dimension": (d2 >> 14) & 3,
                        "offset": (signed((d2 >> 16) & 0x1F, 5), signed((d2 >> 21) & 0x1F, 5)),
                    }
        address_bytes += 12
        cf_index += 1


def overrides(fetch: dict) -> list[str]:
    found = []
    for field in ("mag", "min", "mip"):
        if fetch[field] != 3:
            found.append(f"{field}={FILTERS[fetch[field]]}")
    if fetch["aniso"] != 7:  # 7 = use the fetch constant
        found.append(f"aniso={fetch['aniso']}")
    if fetch["lod_bias"]:
        found.append(f"lod_bias={fetch['lod_bias'] / 16:g}")
    if fetch["offset"] != (0, 0):
        found.append(f"offset={fetch['offset'][0] / 2:g},{fetch['offset'][1] / 2:g}")
    if fetch["denorm"]:
        found.append("denormalized")
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="List texture fetch overrides.")
    parser.add_argument("containers", type=Path, nargs="?", default=Path("cache/guest-shaders"))
    args = parser.parse_args(argv)
    summary = collections.Counter()
    shaders = 0
    for path in sorted(args.containers.glob("*.bin")):
        data = path.read_bytes()
        hits = []
        for slot, fetch in texture_fetches(data):
            found = overrides(fetch)
            for item in found:
                summary[item.split("=")[0]] += 1
            if found:
                hits.append(f"  instr {slot} const {fetch['const']}: {' '.join(found)}")
        if hits:
            shaders += 1
            print(path.stem)
            print("\n".join(hits))
    print(f"\n{shaders} shaders override fetch constant state: {dict(summary)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
