import json
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import latency_tool  # noqa: E402


class LatencyToolTests(unittest.TestCase):
    def test_ntp_offset_and_rtt(self):
        records = [{
            "kind": "clock_sync",
            "device_send_ns": 1_000_000_000,
            "host_receive_ns": 1_003_000_000,
            "host_send_ns": 1_004_000_000,
            "device_receive_ns": 1_010_000_000,
        }]
        report = latency_tool.analyze_clock(records)
        self.assertEqual(report["samples"]["median"], -1.5)
        self.assertEqual(report["rtt"]["median"], 9.0)

    def test_percentiles_and_release_gates(self):
        records = []
        for index in range(20):
            records.append({"kind": "glass_sample", "latency_ms": 40 + index * 0.1})
            records.append({"kind": "input_transport", "latency_ms": 3 + index * 0.05})
        report = latency_tool.analyze_latency(records, 35, 50, 70, 8)
        self.assertAlmostEqual(report["glass_to_glass"]["median"], 40.95)
        self.assertTrue(report["gates"]["release_median_le_50ms"])
        self.assertTrue(report["gates"]["release_p95_le_70ms"])
        self.assertTrue(report["gates"]["input_transport_p95_le_8ms"])
        self.assertFalse(report["optimizationTargetMet"])
        self.assertTrue(report["releasePass"])
        self.assertTrue(report["pass"])

    def test_timestamp_derived_latency(self):
        records = [{
            "kind": "glass_sample",
            "capture_timestamp_ns": 100,
            "presentation_timestamp_ns": 20_100_000,
        }, {"kind": "input_transport", "latency_us": 7500}]
        report = latency_tool.analyze_latency(records, 35, 50, 70, 8)
        self.assertEqual(report["glass_to_glass"]["median"], 20.0999)
        self.assertEqual(report["input_transport"]["median"], 7.5)

    def test_encoder_samples_cannot_pass_glass_release_gate(self):
        records = [{"kind": "video_sample", "capture_timestamp_ns": 100,
                    "presentation_timestamp_ns": 20_100_000,
                    "latency_ms": 20, "glass_to_glass_ms": 20},
                   {"kind": "input_transport", "latency_us": 1000}]
        report = latency_tool.analyze_latency(records, 35, 50, 70, 8)
        self.assertEqual(report["glass_to_glass"]["count"], 0)
        self.assertIsNone(report["gates"]["release_median_le_50ms"])
        self.assertFalse(report["releasePass"])

    def test_soak_passes_and_rejects_failure_conditions(self):
        good = [
            {"kind": "soak", "timestamp_s": 0, "queue_depth": 0, "active_pointer_ids": [], "crashes": 0, "latency_ms": 30},
            {"kind": "soak", "timestamp_s": 1800, "queue_depth": 0, "active_pointer_ids": [1], "crashes": 0, "latency_ms": 31},
            {"kind": "soak", "timestamp_s": 3600, "queue_depth": 0, "active_pointer_ids": [], "crashes": 0, "latency_ms": 30.5},
        ]
        report = latency_tool.analyze_soak(good)
        self.assertTrue(report["pass"])
        bad = list(good)
        bad[-1] = {"kind": "soak", "timestamp_s": 3600, "queue_depth": 2, "active_pointers": 1, "crashes": 1, "latency_ms": 50}
        report = latency_tool.analyze_soak(bad)
        self.assertFalse(report["pass"])
        self.assertFalse(report["gates"]["queue_growth"])
        self.assertFalse(report["gates"]["pointers_released"])
        self.assertFalse(report["gates"]["no_crashes"])
        self.assertFalse(report["gates"]["drift_le_one_frame"])

    def test_jsonl_and_cli_report(self):
        fixture = HERE / "fixtures" / "latency.jsonl"
        records = latency_tool.load_jsonl(str(fixture))
        report = latency_tool.analyze(records)
        self.assertTrue(report["overall_pass"])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            with contextlib.redirect_stdout(io.StringIO()):
                code = latency_tool.main([str(fixture), "--output", str(output)])
            self.assertEqual(code, latency_tool.EXIT_OK)
            self.assertTrue(json.loads(output.read_text(encoding="utf-8"))["overall_pass"])

    def test_optimization_miss_does_not_fail_release(self):
        fixture = HERE / "fixtures" / "optimization_target_miss_release_pass.jsonl"
        report = latency_tool.analyze(latency_tool.load_jsonl(str(fixture)))
        self.assertEqual(report["latency"]["glass_to_glass"]["median"], 40.0)
        self.assertEqual(report["latency"]["input_transport"]["count"], 3)
        self.assertFalse(report["latency"]["optimizationTargetMet"])
        self.assertTrue(report["latency"]["releasePass"])
        self.assertTrue(report["overall_pass"])
        self.assertEqual(report["failures"], [])
        with contextlib.redirect_stdout(io.StringIO()):
            code = latency_tool.main([str(fixture)])
        self.assertEqual(code, latency_tool.EXIT_OK)


if __name__ == "__main__":
    unittest.main()
