"""Check a finished perf capture against the smoothness gate in docs/performance.md.

Reads perf.csv (guest swaps) and present.csv (host presents) from a run directory. Stretches
where the game itself runs at 30 FPS (entrances) are reported separately and left out of the
60 FPS checks.
"""
import argparse
import csv
import json
import math
import statistics
import sys
from pathlib import Path

GATE_FPS = 59.9
GATE_P99_MS = 17.5
GATE_MAX_MS = 50.0
# A rolling median above this marks a scene the game deliberately runs at 30 FPS.
HALF_RATE_MS = 28.0
WINDOW = 31


def load(path, column, time_column=None):
    """Return (end_seconds, interval_us) pairs with positive intervals."""
    rows, elapsed = [], 0.0
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            # A capture cut off mid-write ends in a partial row.
            if not row.get(column) or (time_column and not row.get(time_column)):
                continue
            interval = int(row[column])
            if time_column:
                elapsed = int(row[time_column]) / 1e6
            else:
                elapsed += interval / 1e6
            if interval > 0:
                rows.append((elapsed, interval))
    return rows


def split_half_rate(rows):
    """Split rows into (full-rate, half-rate) by a centred rolling median of intervals."""
    full, half = [], []
    intervals = [interval for _, interval in rows]
    radius = WINDOW // 2
    for i, row in enumerate(rows):
        neighbourhood = intervals[max(0, i - radius):i + radius + 1]
        (half if statistics.median(neighbourhood) / 1000 > HALF_RATE_MS else full).append(row)
    return full, half


def stats(rows):
    if not rows:
        return None
    ordered = sorted(interval for _, interval in rows)
    total = sum(ordered)
    pct = lambda p: ordered[max(0, math.ceil(len(ordered) * p) - 1)] / 1000
    return {"frames": len(ordered), "seconds": round(total / 1e6, 2),
            "fps": round(len(ordered) * 1e6 / total, 3),
            "median_ms": round(statistics.median(ordered) / 1000, 3),
            "p99_ms": round(pct(.99), 3), "max_ms": round(ordered[-1] / 1000, 3),
            "over_50_ms": sum(t > GATE_MAX_MS * 1000 for t in ordered)}


def half_rate_spans(rows):
    spans = []
    for end, _ in rows:
        if spans and end - spans[-1][1] <= 1.0:
            spans[-1][1] = end
        else:
            spans.append([end, end])
    return [[round(start, 1), round(last, 1)] for start, last in spans]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="analysis/vulkan-play-* directory")
    parser.add_argument("--warmup", type=float, default=20.0,
                        help="Seconds at the start excluded from every check (boot, cache load)")
    args = parser.parse_args()
    report, failures = {"run": str(args.run), "warmup_seconds": args.warmup}, []
    sources = {"guest_swaps": (args.run / "perf.csv", "frame_time_us", None),
               "host_presents": (args.run / "present.csv", "interval_us", "present_us")}
    for name, (path, column, time_column) in sources.items():
        if not path.exists():
            failures.append(f"{name}: {path.name} missing")
            continue
        rows = [r for r in load(path, column, time_column) if r[0] >= args.warmup]
        full, half = split_half_rate(rows)
        report[name] = {"full_rate": stats(full), "half_rate": stats(half),
                        "half_rate_spans_seconds": half_rate_spans(half)}
        s = report[name]["full_rate"]
        if not s:
            failures.append(f"{name}: no frames after warm-up")
            continue
        if s["fps"] < GATE_FPS:
            failures.append(f"{name}: {s['fps']} FPS < {GATE_FPS}")
        if s["over_50_ms"]:
            failures.append(f"{name}: {s['over_50_ms']} frames over {GATE_MAX_MS} ms")
        # Guest swaps jitter by design (13-21 ms); only presentation must be even.
        if name == "host_presents" and s["p99_ms"] > GATE_P99_MS:
            failures.append(f"{name}: p99 {s['p99_ms']} ms > {GATE_P99_MS} ms")
    report["failures"] = failures
    report["verdict"] = "PASS" if not failures else "FAIL"
    print(json.dumps(report, indent=2))
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
