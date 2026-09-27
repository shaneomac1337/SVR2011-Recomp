"""Summarize guest swap intervals after the game closes its perf.csv file."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def summarize(intervals_us, target_fps=60):
    if not intervals_us:
        raise ValueError("No positive frame intervals in the selected range")
    ordered = sorted(intervals_us)
    total = sum(ordered)
    percentile = lambda p: ordered[math.ceil(len(ordered) * p) - 1] / 1000
    return {
        "frames": len(ordered),
        "interval_seconds": round(total / 1_000_000, 4),
        "guest_fps": round(len(ordered) * 1_000_000 / total, 3),
        "median_ms": round(statistics.median(ordered) / 1000, 3),
        "p95_ms": round(percentile(.95), 3),
        "p99_ms": round(percentile(.99), 3),
        "max_ms": round(ordered[-1] / 1000, 3),
        "over_two_frame_budgets": sum(t > 2_000_000 / target_fps for t in ordered),
        "over_100_ms": sum(t > 100_000 for t in ordered),
    }


def read_intervals(path, start=0, end=math.inf):
    elapsed = 0.0
    selected = []
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            duration = int(row["frame_time_us"])
            if duration <= 0:
                continue
            elapsed += duration / 1_000_000
            # A frame belongs to the window containing its ending timestamp.
            if start <= elapsed < end:
                selected.append(duration)
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--start", type=float, default=0, help="Seconds since first measured swap")
    parser.add_argument("--end", type=float, default=math.inf)
    parser.add_argument("--target-fps", type=float, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.start) or args.start < 0 or args.end <= args.start or math.isnan(args.end):
        parser.error("Require 0 <= start < end")
    if not math.isfinite(args.target_fps) or args.target_fps <= 0:
        parser.error("target-fps must be positive and finite")
    try:
        report = summarize(read_intervals(args.csv, args.start, args.end), args.target_fps)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Cannot analyze capture (close the game first): {error}\n")
    print(json.dumps({"capture": str(args.csv), "start_seconds": args.start,
                      "end_seconds": args.end if math.isfinite(args.end) else None,
                      "target_fps": args.target_fps, **report}, indent=2))


if __name__ == "__main__":
    main()
