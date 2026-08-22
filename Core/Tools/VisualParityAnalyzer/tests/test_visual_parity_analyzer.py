import sys
import tempfile
import unittest
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parents[1]
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

from visual_parity_analyzer import AnalysisOptions, analyze_directories


def write_ppm(path, width, height, pixels):
    with path.open("wb") as output:
        output.write(("P6\n{} {}\n255\n".format(width, height)).encode("ascii"))
        output.write(bytes(channel for pixel in pixels for channel in pixel))


def solid(width, height, color):
    return [color] * (width * height)


class VisualParityAnalyzerTests(unittest.TestCase):
    def test_one_frame_object_flicker_is_reported_in_low_motion_regions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(4, 4, (0, 0, 0))
            flicker = list(black)
            flicker[5] = (255, 0, 0)
            for index, pixels in enumerate((black, black, black)):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 4, 4, pixels)
            for index, pixels in enumerate((black, flicker, black)):
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 4, 4, pixels)

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(low_motion_threshold=0.0, flicker_threshold=0.02),
            )

            self.assertEqual(report["alignment"]["paired_frame_count"], 3)
            self.assertGreater(report["frames"][1]["image_error"]["mae"], 0.0)
            transitions = report["temporal"]["transitions"]
            self.assertEqual(len(transitions), 2)
            self.assertGreater(transitions[0]["flicker_pixel_ratio"], 0.0)
            self.assertGreater(transitions[1]["flicker_pixel_ratio"], 0.0)
            self.assertGreater(transitions[0]["flicker_residual"]["max"], 0.0)
            self.assertGreater(transitions[1]["flicker_residual"]["max"], 0.0)
            self.assertEqual(report["temporal"]["flicker_transition_count"], 2)
            self.assertGreater(report["temporal"]["low_motion_pixel_ratio"]["mean"], 0.0)

    def test_duplicate_frame_is_reported_for_candidate_sequence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(2, 2, (0, 0, 0))
            red = solid(2, 2, (255, 0, 0))
            blue = solid(2, 2, (0, 0, 255))
            for index, pixels in enumerate((black, red, blue)):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 2, 2, pixels)
            for index, pixels in enumerate((black, black, blue)):
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 2, 2, pixels)

            report = analyze_directories(legacy, candidate)

            duplicate = report["cadence"]["candidate"]["duplicates"]
            self.assertEqual(duplicate["duplicate_frame_count"], 1)
            self.assertEqual(len(duplicate["runs"]), 1)
            self.assertEqual(duplicate["runs"][0]["start_frame"], 0)
            self.assertEqual(duplicate["runs"][0]["end_frame"], 1)

    def test_timestamp_gap_reports_estimated_drop_and_irregular_cadence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            frames = (
                solid(1, 1, (0, 0, 0)),
                solid(1, 1, (1, 1, 1)),
                solid(1, 1, (2, 2, 2)),
            )
            for index, pixels in enumerate(frames):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
            legacy_times = root / "legacy.times"
            candidate_times = root / "candidate.times"
            legacy_times.write_text("0\n1\n2\n", encoding="utf-8")
            candidate_times.write_text("0\n1\n3\n", encoding="utf-8")

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(
                    legacy_timestamps=legacy_times,
                    candidate_timestamps=candidate_times,
                    cadence_tolerance=0.10,
                ),
            )

            cadence = report["cadence"]["candidate"]
            self.assertEqual(cadence["timestamps"]["estimated_dropped_frames"], 1)
            self.assertEqual(cadence["timestamps"]["irregular_interval_count"], 1)
            self.assertAlmostEqual(cadence["timestamps"]["fps"]["observed"], 2.0 / 3.0)
            self.assertAlmostEqual(cadence["timestamps"]["fps"]["min"], 0.5)
            self.assertAlmostEqual(cadence["timestamps"]["nominal_interval"], 1.0)

    def test_timestamp_pairing_skips_extra_candidate_sample(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(1, 1, (0, 0, 0))
            red = solid(1, 1, (255, 0, 0))
            gray = solid(1, 1, (127, 127, 127))
            for index, pixels in enumerate((black, red)):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
            for index, pixels in enumerate((black, gray, red)):
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
            legacy_times = root / "legacy.times"
            candidate_times = root / "candidate.times"
            legacy_times.write_text("0\n1\n", encoding="utf-8")
            candidate_times.write_text("0\n0.5\n1\n", encoding="utf-8")

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(legacy_timestamps=legacy_times, candidate_timestamps=candidate_times),
            )

            self.assertEqual(report["alignment"]["paired_frame_count"], 2)
            self.assertEqual(report["frames"][1]["candidate_file"], "frame_002.ppm")
            self.assertAlmostEqual(report["frames"][1]["image_error"]["mae"], 0.0)
            self.assertEqual(report["alignment"]["timestamp_pairing"]["unmatched_candidate_frame_count"], 1)

    def test_duplicate_report_exposes_held_frames_and_duration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(1, 1, (0, 0, 0))
            red = solid(1, 1, (255, 0, 0))
            for index, pixels in enumerate((black, red, black, red)):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
            timestamps = root / "timestamps.times"
            timestamps.write_text("0\n0.5\n1\n1.5\n", encoding="utf-8")

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(legacy_timestamps=timestamps, candidate_timestamps=timestamps),
            )

            duplicate = report["cadence"]["candidate"]["duplicates"]
            self.assertEqual(duplicate["held_frame_count"], 0)
            self.assertEqual(duplicate["held_run_count"], 0)

            held_candidate = root / "held-candidate"
            held_candidate.mkdir()
            for index, pixels in enumerate((black, black, red, red)):
                write_ppm(held_candidate / "frame_{:03d}.ppm".format(index), 1, 1, pixels)
            held_report = analyze_directories(
                legacy,
                held_candidate,
                AnalysisOptions(legacy_timestamps=timestamps, candidate_timestamps=timestamps),
            )
            held = held_report["cadence"]["candidate"]["duplicates"]
            self.assertEqual(held["held_frame_count"], 2)
            self.assertEqual(held["held_run_count"], 2)
            self.assertAlmostEqual(held["runs"][0]["held_duration_seconds"], 1.0)

    def test_nonfinite_timestamp_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            legacy.mkdir()
            candidate.mkdir()
            pixels = solid(1, 1, (0, 0, 0))
            write_ppm(legacy / "frame_000.ppm", 1, 1, pixels)
            write_ppm(candidate / "frame_000.ppm", 1, 1, pixels)
            timestamps = root / "timestamps.times"
            timestamps.write_text("nan\n", encoding="utf-8")

            with self.assertRaises(ValueError):
                analyze_directories(
                    legacy,
                    candidate,
                    AnalysisOptions(legacy_timestamps=timestamps, candidate_timestamps=timestamps),
                )

    def test_diagnostic_heatmaps_are_optional_portable_ppm_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            diagnostics = root / "diagnostics"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(2, 2, (0, 0, 0))
            white = solid(2, 2, (255, 255, 255))
            write_ppm(legacy / "frame_000.ppm", 2, 2, black)
            write_ppm(candidate / "frame_000.ppm", 2, 2, white)

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(diagnostic_dir=diagnostics, diagnostic_limit=1),
            )

            self.assertEqual(len(report["diagnostics"]), 1)
            diagnostic = Path(report["diagnostics"][0])
            self.assertEqual(diagnostic.suffix, ".ppm")
            self.assertTrue(diagnostic.is_file())
            self.assertTrue(diagnostic.read_bytes().startswith(b"P6\n2 2\n255\n"))

    def test_diagnostic_heatmaps_include_temporal_flicker(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            legacy = root / "legacy"
            candidate = root / "candidate"
            diagnostics = root / "diagnostics"
            legacy.mkdir()
            candidate.mkdir()
            black = solid(2, 2, (0, 0, 0))
            flicker = list(black)
            flicker[0] = (255, 0, 0)
            for index, pixels in enumerate((black, black, black)):
                write_ppm(legacy / "frame_{:03d}.ppm".format(index), 2, 2, pixels)
            for index, pixels in enumerate((black, flicker, black)):
                write_ppm(candidate / "frame_{:03d}.ppm".format(index), 2, 2, pixels)

            report = analyze_directories(
                legacy,
                candidate,
                AnalysisOptions(diagnostic_dir=diagnostics, diagnostic_limit=2),
            )

            names = {Path(path).name for path in report["diagnostics"]}
            self.assertIn("frame_000000_error.ppm", names)
            self.assertIn("transition_000001_flicker.ppm", names)


if __name__ == "__main__":
    unittest.main()
