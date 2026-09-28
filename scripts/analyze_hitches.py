"""List slow frames in a hitch session and the runtime events logged around each one.

A frame is slow when its present interval exceeds the scene's normal interval (the median of
its neighbours, so 30 FPS entrances are not flagged) by more than --slack milliseconds. Each
slow frame is printed with the pipeline builds, skipped presents and blocking readbacks logged
within --window milliseconds before it. With marks.txt present (scripts/mark.ps1), only frames
from --before seconds before a mark to --after seconds after it are reported.
"""
import argparse
import csv
import datetime
import re
import statistics
from pathlib import Path

LOG_TIME = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
ORIGIN = "Paced presentation log started"
EVENTS = {
    "Creating graphics pipeline": "pipeline build",
    "Frame event: present skipped": "present skipped (pipeline building)",
    "Frame event: blocking resolve readback": "blocking readback",
}


def parse_time(text):
    return datetime.datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")


def load_log(path):
    origin, events = None, []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LOG_TIME.match(line)
        if not match:
            continue
        if ORIGIN in line and origin is None:
            origin = parse_time(match.group(1))
        for needle, kind in EVENTS.items():
            if needle in line:
                events.append((parse_time(match.group(1)), kind, line[match.end():].strip()))
    return origin, events


def load_presents(path, origin):
    rows = []
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            if not row.get("interval_us") or int(row["interval_us"]) <= 0:
                continue
            at = origin + datetime.timedelta(microseconds=int(row["present_us"]))
            rows.append((at, int(row["interval_us"]) / 1000))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="analysis/hitch-* folder")
    parser.add_argument("--slack", type=float, default=4.0, help="ms over the scene's normal interval")
    parser.add_argument("--window", type=float, default=250.0, help="ms of log events before a frame")
    parser.add_argument("--before", type=float, default=2.0, help="seconds before a mark")
    parser.add_argument("--after", type=float, default=20.0, help="seconds after a mark")
    args = parser.parse_args()

    origin, events = load_log(args.run / "runtime.log")
    if origin is None:
        parser.exit(1, "No paced presentation log in this run (frame pacing must be on).\n")
    presents = load_presents(args.run / "present.csv", origin)
    marks = []
    marks_path = args.run / "marks.txt"
    if marks_path.exists():
        for line in marks_path.read_text(encoding="utf-8").splitlines():
            stamp, _, label = line.partition("\t")
            marks.append((parse_time(stamp), label))

    def in_scope(at):
        if not marks:
            return True
        return any(-args.before <= (at - m).total_seconds() <= args.after for m, _ in marks)

    intervals = [ms for _, ms in presents]
    slow = []
    for i, (at, ms) in enumerate(presents):
        normal = statistics.median(intervals[max(0, i - 15):i + 16])
        if ms > normal + args.slack and in_scope(at):
            slow.append((at, ms, normal))

    for mark, label in marks:
        print(f"MARK {mark:%H:%M:%S.%f}"[:-3] + f"  {label}")
    print(f"{len(slow)} slow frames" + (" near marks" if marks else ""))
    for at, ms, normal in slow:
        near = [e for e in events if 0 <= (at - e[0]).total_seconds() * 1000 <= args.window]
        mark_note = ""
        if marks:
            mark, label = min(marks, key=lambda m: abs((at - m[0]).total_seconds()))
            mark_note = f"  ({(at - mark).total_seconds():+.2f} s from {label})"
        print(f"\n{at:%H:%M:%S.%f}"[:-3] + f"  {ms:.1f} ms (normal {normal:.1f}){mark_note}")
        counts = {}
        for _, kind, _ in near:
            counts[kind] = counts.get(kind, 0) + 1
        if not near:
            print("  no logged events: not a pipeline build or readback (check CPU-side work)")
        for kind, count in counts.items():
            print(f"  {count} x {kind}")


if __name__ == "__main__":
    main()
