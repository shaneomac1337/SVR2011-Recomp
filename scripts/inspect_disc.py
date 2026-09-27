"""Read an Xbox disc without mounting it; inventory files and extract default.xex.

Uses only Python's standard library. Outputs are local inputs, not source code.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"MICROSOFT*XBOX*MEDIA"
BASES = (0, 0xFD90000, 0x2080000, 0x18300000)
SECTOR = 2048


class Disc:
    def __init__(self, path):
        self.path = Path(path)
        self.stream = self.path.open("rb")
        self.size = self.path.stat().st_size
        try:
            for base in BASES:
                if base + 0x10800 > self.size:
                    continue
                header = self.read(base + 0x10000, SECTOR)
                if header[:20] == MAGIC and header[-20:] == MAGIC:
                    self.base = base
                    self.root_sector, self.root_size = struct.unpack_from("<II", header, 20)
                    break
            else:
                raise ValueError("No supported XDVDFS volume found")
        except Exception:
            self.close()
            raise

    def close(self):
        self.stream.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError(f"Read outside image: offset={offset}, size={size}")
        self.stream.seek(offset)
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError("Truncated image")
        return data

    def files(self):
        result = []
        directories = set()

        def directory(sector, size, parent, depth):
            if depth > 64 or sector in directories:
                raise ValueError("Cyclic or excessively deep directory structure")
            if size == 0:
                return
            directories.add(sector)
            if size > 16 * 1024 * 1024:
                raise ValueError("Unreasonably large directory table")
            data = self.read(self.base + sector * SECTOR, size)
            pending, visited = [0], set()
            while pending:
                offset = pending.pop()
                if offset in visited or offset + 14 > size:
                    raise ValueError("Invalid directory entry pointer")
                visited.add(offset)
                left, right, entry_sector, length, attributes, name_len = struct.unpack_from(
                    "<HHIIBB", data, offset
                )
                if not name_len or offset + 14 + name_len > size:
                    raise ValueError("Invalid entry name length")
                name = data[offset + 14:offset + 14 + name_len].decode("ascii")
                if name in (".", "..") or any(c in name for c in '/\\:\x00'):
                    raise ValueError("Unsafe entry name")
                relative = f"{parent}/{name}" if parent else name
                absolute = self.base + entry_sector * SECTOR
                if absolute + length > self.size:
                    raise ValueError(f"Entry outside image: {relative}")
                if attributes & 0x10:
                    directory(entry_sector, length, relative, depth + 1)
                else:
                    result.append({"path": relative, "offset": absolute, "size": length})
                pending.extend(pointer * 4 for pointer in (right, left) if pointer)

        directory(self.root_sector, self.root_size, "", 0)
        paths = [entry["path"].lower() for entry in result]
        if len(paths) != len(set(paths)):
            raise ValueError("Duplicate file names in disc")
        return sorted(result, key=lambda entry: entry["path"].lower())

    def extract(self, entry, root):
        root = Path(root).resolve()
        target = (root / entry["path"]).resolve()
        if not target.is_relative_to(root) or target == root:
            raise ValueError("Extraction destination escapes asset directory")
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        remaining = entry["size"]
        self.stream.seek(entry["offset"])
        if target.exists():
            # A repeated extraction verifies existing files, never overwrites them.
            with target.open("rb") as existing:
                while remaining:
                    block = self.stream.read(min(1024 * 1024, remaining))
                    if not block or existing.read(len(block)) != block:
                        raise ValueError(f"Existing file differs from image: {target}")
                    digest.update(block)
                    remaining -= len(block)
                if existing.read(1):
                    raise ValueError(f"Existing file differs from image: {target}")
        else:
            with target.open("xb") as output:
                try:
                    while remaining:
                        block = self.stream.read(min(1024 * 1024, remaining))
                        if not block:
                            raise ValueError("Truncated image during extraction")
                        output.write(block)
                        digest.update(block)
                        remaining -= len(block)
                except BaseException:
                    output.close()
                    target.unlink()  # Only the incomplete file this call created.
                    raise
        return digest.hexdigest()


def inspect_xex(data):
    if data[:4] != b"XEX2" or len(data) < 24:
        raise ValueError("Expected an XEX2 executable")
    flags, pe_offset, reserved, security_offset, count = struct.unpack_from(
        ">IIIII", data, 4
    )
    if 24 + count * 8 > len(data):
        raise ValueError("Truncated XEX optional headers")
    info = {
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data), "module_flags": f"0x{flags:08X}",
        "pe_data_offset": pe_offset, "security_offset": security_offset,
        "optional_headers": [],
    }
    for index in range(count):
        key, value = struct.unpack_from(">II", data, 24 + index * 8)
        info["optional_headers"].append({"key": f"0x{key:08X}", "value": f"0x{value:08X}"})
        if key in (0x10100, 0x10201):
            info["entry_point" if key == 0x10100 else "image_base"] = f"0x{value:08X}"
        elif key == 0x40006:
            media, version, base_version, title = struct.unpack_from(">IIII", data, value)
            info.update(title_id=f"{title:08X}", media_id=f"{media:08X}",
                        version=f"{version:08X}", base_version=f"{base_version:08X}")
    return info


def validate_destinations(iso, report, executable=None, assets=None):
    """Keep reports separate from both the input image and extracted game data."""
    source = iso.resolve()
    report = report.resolve()
    if report == source:
        raise ValueError("Report path must not be the input image")
    if executable is not None:
        executable = executable.resolve()
        if executable == source:
            raise ValueError("Executable destination must not be the input image")
        if report == executable:
            raise ValueError("Report path must not be the executable destination")
    if assets is not None and report.is_relative_to(assets.resolve()):
        raise ValueError("Report path must be outside the extracted asset directory")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--report", type=Path, default=Path("analysis/disc.json"))
    parser.add_argument("--extract-executable", type=Path)
    parser.add_argument("--extract-all", type=Path, help="Extract/verify all files under this directory")
    args = parser.parse_args()
    try:
        validate_destinations(args.iso, args.report, args.extract_executable, args.extract_all)
    except ValueError as error:
        parser.error(str(error))
    disc = Disc(args.iso)
    try:
        files = disc.files()
        executable = next(entry for entry in files if entry["path"].lower() == "default.xex")
        data = disc.read(executable["offset"], executable["size"])
        report = {
            "image": args.iso.name, "image_size": disc.size,
            "partition_base": disc.base, "file_count": len(files),
            "executable": inspect_xex(data), "files": files,
        }
        if args.extract_all:
            for index, entry in enumerate(files, 1):
                entry["sha256"] = disc.extract(entry, args.extract_all)
                print(f"[{index}/{len(files)}] {entry['path']}", flush=True)
        if args.extract_executable:
            target = args.extract_executable
            if target.exists():
                if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(data).digest():
                    raise ValueError(f"Refusing to replace different file: {target}")
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("xb") as output:
                    output.write(data)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: value for key, value in report.items() if key != "files"}, indent=2))
    finally:
        disc.close()


if __name__ == "__main__":
    main()
