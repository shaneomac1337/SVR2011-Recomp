import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("analyze_perf", Path(__file__).parents[1] / "scripts/analyze_perf.py")
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)


class PerfTests(unittest.TestCase):
    def test_fps_uses_elapsed_time_not_average_instantaneous_fps(self):
        report = perf.summarize([10_000, 90_000])
        self.assertEqual(report["guest_fps"], 20)
        self.assertEqual(report["median_ms"], 50)
        self.assertEqual(report["p95_ms"], 90)
        self.assertEqual(report["over_two_frame_budgets"], 1)

    def test_interval_selection_excludes_zero_and_uses_end_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "perf.csv"
            with path.open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow(["frame_time_us"])
                writer.writerows([[0], [10_000], [20_000], [30_000]])
            self.assertEqual(perf.read_intervals(path, .015, .05), [20_000])

    def test_empty_capture_is_not_reported_as_success(self):
        with self.assertRaises(ValueError):
            perf.summarize([])
