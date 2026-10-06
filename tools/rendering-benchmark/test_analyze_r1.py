"""Small synthetic contract checks; these do not measure game performance."""
import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import analyze_r1 as r1

EXPECTED_R2_PROFILE_ROSTERS = {
    "combined_arms_256": {
        "roster_contract": "ggc.r2.rendered-battle.roster.combined-arms-256.v1",
        "units_per_player": 32, "template_counts": (4, 4, 12, 12),
        "unit_type_sequence": "00001111222222222222333333333333",
    },
    "combined_arms_512": {
        "roster_contract": "ggc.r2.rendered-battle.roster.combined-arms-512.v1",
        "units_per_player": 64, "template_counts": (8, 8, 24, 24),
        "unit_type_sequence": "0123012301230123012301230123012323232323232323232323232323232323",
    },
    "mechanized_256": {
        "roster_contract": "ggc.r2.rendered-battle.roster.mechanized-256.v1",
        "units_per_player": 32, "template_counts": (16, 16, 0, 0),
        "unit_type_sequence": "00000000000000001111111111111111",
    },
    "mechanized_512": {
        "roster_contract": "ggc.r2.rendered-battle.roster.mechanized-512.v1",
        "units_per_player": 64, "template_counts": (32, 32, 0, 0),
        "unit_type_sequence": "0000000000000000000000000000000011111111111111111111111111111111",
    },
    "infantry_line_256": {
        "roster_contract": "ggc.r2.rendered-battle.roster.infantry-line-256.v1",
        "units_per_player": 32, "template_counts": (0, 0, 16, 16),
        "unit_type_sequence": "22222222222222223333333333333333",
    },
    "infantry_line_512": {
        "roster_contract": "ggc.r2.rendered-battle.roster.infantry-line-512.v1",
        "units_per_player": 64, "template_counts": (0, 0, 32, 32),
        "unit_type_sequence": "2222222222222222222222222222222233333333333333333333333333333333",
    },
}


def make_csv(path, ends, positive=(), export_delta=0, failed=()):
    rows = []
    for i, end in enumerate(ends, 1):
        is_positive = i in positive
        is_failed = i in failed
        rows.append({
            "row_type": "present", "owner_session": 1, "process_id": 77,
            "owner_thread_id": 2, "owner_start_qpc": 50, "device_epoch": 1,
            "backend_frame_ordinal": i, "present_ordinal": i,
            "present_start_qpc": end - 1, "present_end_qpc": end,
            "cpu_qpc_frequency": 1000, "present_hresult": -1 if is_failed else (1 if is_positive else 0),
            "swap_interval": 0, "present_flags": 0, "width": 1920, "height": 1080,
            "status": "failed" if is_failed else ("positive" if is_positive else "s_ok"), "count": "",
        })
    counters = {
        "capacity": 32768, "calls": len(ends), "retained": len(ends),
        "dropped": 0, "s_ok": len(ends)-len(positive)-len(failed), "positive": len(positive),
        "failed": len(failed), "invalid_clocks": 0, "io_failures": 0,
        "export_end": len(ends)+export_delta,
    }
    for key, value in counters.items():
        row = {column: "" for column in r1.HEADER}
        row.update(row_type="summary", owner_session=1, process_id=77,
                   owner_thread_id=2, owner_start_qpc=50, status=key, count=value)
        rows.append(row)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=r1.HEADER)
        writer.writeheader()
        writer.writerows(rows)


def gpu_frame(epoch=1, ordinal=1, start=10500, end=10510, **overrides):
    row = {column: "" for column in r1.GPU_HEADER}
    row.update(row_type="frame", device_epoch=epoch, local_frame_ordinal=ordinal,
               status="complete", width=1920, height=1080, samples=8, gamma=1,
               brightness=0, contrast=1, gamma_limit=1, gamma_applied=0,
               resolve_applied=1, swap_interval=0, present_flags=0,
               present_called=1, present_hresult=0, cpu_present_ms=1.0,
               readback_contaminated=0, gpu_total_ms=2.0, gpu_scene_ms=1.5,
               gpu_resolve_ms=0.5, gpu_gamma_ms=0, gpu_elapsed_not_busy=1,
               owner_begin_tick_ms=10, present_start_qpc=start,
               present_end_qpc=end, cpu_qpc_frequency=1000, count="")
    row.update(overrides)
    return row


def gpu_present_row(epoch=1, ordinal=1, start=10500, end=10510, **overrides):
    row = {"owner_session": 3, "process_id": 77, "device_epoch": epoch,
           "backend_frame_ordinal": ordinal, "present_ordinal": ordinal,
           "present_start_qpc": start, "present_end_qpc": end,
           "cpu_qpc_frequency": 1000, "present_hresult": 0,
           "width": 1920, "height": 1080}
    row.update(overrides)
    return row


def write_gpu_csv(path, frames, adapters=None, counters=None, omit_export_end=False,
                  truncate_export_end=False):
    adapters = adapters or {}
    rows = list(frames)
    for epoch in sorted({int(row["device_epoch"]) for row in frames}):
        values = {"adapter_identity_valid": 1, "adapter_vendor_id": 4318,
                  "adapter_device_id": 7956, "adapter_luid_low": 88384,
                  "adapter_luid_high": 0, "adapter_software": 0,
                  "device_feature_level": 45056, "device_debug_layer": 0}
        values.update(adapters.get(epoch, {}))
        for name in r1.GPU_DEVICE_SUMMARIES:
            rows.append(gpu_summary(epoch, name, values[name]))
    values = {name: 0 for name in r1.GPU_COUNTER_SUMMARIES}
    values.update(frames_begun=len(frames), records=len(frames),
                  complete=sum(row["status"] == "complete" for row in frames),
                  invalid=sum(row["status"] in ("invalid", "disjoint", "readiness_failed", "unpresented") for row in frames),
                  disjoint=sum(row["status"] == "disjoint" for row in frames),
                  shutdown_pending=sum(row["status"] == "shutdown_pending" for row in frames),
                  export_end=len(frames))
    if counters:
        values.update(counters)
    for name in r1.GPU_COUNTER_SUMMARIES:
        if name == "export_end" and omit_export_end:
            continue
        rows.append(gpu_summary(0, name, values[name]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=r1.GPU_HEADER)
        writer.writeheader()
        writer.writerows(rows)
    if truncate_export_end:
        lines = path.read_text(encoding="utf-8").splitlines()
        lines[-1] = lines[-1].rsplit(",", 1)[0]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def gpu_summary(epoch, name, count):
    row = {column: "" for column in r1.GPU_HEADER}
    row.update(row_type="summary", device_epoch=epoch, local_frame_ordinal=0,
               status=name, count=count)
    return row


def comparison_run(role, number, mode="foreground", state=None, fps=15.0):
    pid = (1000 if role == "baseline" else 2000) + number
    start_qpc = pid * 1000
    return {
        "qualified": True, "run_directory": f"{role}-{number}",
        "identity": {"source_commit": role, "snapshot_identity": role,
                     "controller_script_sha256": "a" * 64,
                     "shadow_multipage_arena": "default",
                     "game_sha256": role, "launcher_sha256": "launch",
                     "telemetry": "minimalPresent", "source_options_sha256": "src",
                     "test_options_sha256": "opts", "settings": {"aa": 8},
                     "affinity": {"mask": "0xFFF"},
                     "captured_affinity": {"game": "0xFFF", "launcher": "0xFFF"},
                     "launcher_configuration": {"path": f"C:\\benchmark-runs\\{role}-{number}\\runtime\\launcher.lcf",
                                                "sha256": "a" * 64,
                                                "content": "RUN = . generalszh.exe"},
                     "window_mode": mode, "present_swap_interval": 0},
        "workload_identity": {"seed": 637808953, "map": "arena", "map_crc": "abc",
                              "map_size": 10, "camera": "(1,2,3)", "roster": [],
                              "state": {"live": 512} if state is None else state},
        "phase": {"qpc_frequency": 1000, "begin": start_qpc + 10000,
                  "stop": start_qpc + 14000},
        "present": {"owner": {"owner_session": 3, "process_id": pid,
                               "owner_thread_id": 2, "owner_start_qpc": start_qpc},
                    "fps": fps, "per_second_fps": [fps],
                    "gaps_ms": {"p95": 70.0, "p99": 80.0, "stall_count": 0},
                    "phase_edge_gap_ms": {"first": 10, "last": 10}},
    }


class SparseSampleTests(unittest.TestCase):
    def valid_sample(self):
        return {"alive0": "4", "alive1": "3", "health0": "100.5", "health1": "99",
                "position_sum0": "10.25", "position_sum1": "11", "attacking": "2",
                "camera": "combat"}

    def test_missing_sparse_fields_are_rejected(self):
        for field in ("alive0", "health1", "position_sum0", "attacking", "camera"):
            with self.subTest(field=field):
                sample = self.valid_sample()
                del sample[field]
                with self.assertRaises(r1.Reject):
                    r1.sparse_sample(sample, 151)

    def test_empty_nonnumeric_and_nonfinite_sparse_values_are_rejected(self):
        for field, value in (("position_sum0", ""), ("attacking", "many"),
                             ("health0", "nan"), ("position_sum1", "inf")):
            with self.subTest(field=field, value=value):
                sample = self.valid_sample()
                sample[field] = value
                with self.assertRaises(r1.Reject):
                    r1.sparse_sample(sample, 151)

    def test_only_combat_camera_marker_is_accepted(self):
        sample = self.valid_sample()
        sample["camera"] = "unknown"
        with self.assertRaisesRegex(r1.Reject, "sample.camera must be combat"):
            r1.sparse_sample(sample, 151)


class VisualCaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".visual-capture-")
        self.root = Path(self.tmp.name)
        profile = self.root / "profile"
        profile.mkdir()
        self.path = profile / "RenderedBattleDiagnostic-synthetic.txt"
        self.ready = {"game": {"sha256": "a" * 64}, "snapshot_identity": "b" * 64,
                      "arguments": ["-runRenderedBattleBenchmark", "637808953"],
                      "test_options": str(profile / "Options.ini"), "telemetry": "presentGpu"}
        self.lines = [
            "RENDERED_BATTLE_DIAGNOSTIC_BEGIN seed=637808953 map=Fortress_Avalanche executable_sha256_observed=" + "a" * 64 + " source_sha256_supplied=" + "b" * 64,
            "RENDERED_BATTLE_DIAGNOSTIC_START seed=637808953 expected_players=8 expected_ai=7 expected_teams=4v4 pacing=render_uncapped_logic30 map=Fortress_Avalanche",
            "RENDERED_BATTLE_DIAGNOSTIC_STAGED created=512 frame=1 camera=(1,2,3) yaw=default pitch=default zoom=1 scripted_camera=stopped camera_lock=none",
            "RENDERED_BATTLE_DIAGNOSTIC_PREFLIGHT_SUMMARY map_crc=ABC map_size=10",
            "RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin frame=1 warmup_frames=150 measure_frames=450 qpc_frequency=1000 fps_source=present_trace render_pacing=uncapped resolution=1920x1080 windowed=1",
            "RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_begin frame=151 qpc=10000 qpc_frequency=1000 alive0=256 alive1=256 attacking=512",
            "RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop frame=601 qpc=14000 qpc_frequency=1000 complete=1 actual_measure_frames=450 measured_attack_samples=450 alive0=256 alive1=256 attacking=512",
        ]
        factions = ("FactionAmerica", "FactionChina", "FactionGLA", "FactionAmerica")
        templates = (
            "AmericaTankCrusader:8,AmericaVehicleHumvee:8,AmericaInfantryRanger:24,AmericaInfantryMissileDefender:24",
            "ChinaTankBattleMaster:8,ChinaTankGattling:8,ChinaInfantryRedguard:24,ChinaInfantryTankHunter:24",
            "GLATankScorpion:8,GLAVehicleTechnical:8,GLAInfantryRebel:24,GLAInfantryTunnelDefender:24",
        )
        for slot in range(8):
            ids = ",".join(str(value) for value in range(slot * 64 + 1, slot * 64 + 65))
            self.lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_ROSTER slot={slot} faction={factions[slot % 4]} team={int(slot >= 4)} count=64 ids={ids} templates={templates[0 if slot % 4 == 3 else slot % 4]}")
        for frame in (151, 301, 451, 601):
            self.lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_SAMPLE frame={frame} alive0=256 alive1=256 health0=100 health1=100 position_sum0=10 position_sum1=10 attacking=512 camera=combat")
        self.lines.append("RENDERED_BATTLE_DIAGNOSTIC_COMPLETE reason=frame_cap terminal_sample=fresh_cap_sample created=512 engine_exit_code=0 diagnostic_exit_code=0 executable_sha256=" + "a" * 64)
        self.legacy_lines = list(self.lines)

    def profile_fixture(self, profile_id):
        expected = EXPECTED_R2_PROFILE_ROSTERS[profile_id]
        per_player = expected["units_per_player"]
        total = per_player * 8
        template_counts = expected["template_counts"]
        roster_contract = expected["roster_contract"]
        phase_contract = r1.R2_PHASE_CONTRACT
        alive = total // 2
        lines = [self.legacy_lines[0],
                 f"RENDERED_BATTLE_BENCHMARK_PROFILE schema={r1.R2_PROFILE_SCHEMA} id={profile_id} roster_contract={roster_contract} phase_contract={phase_contract} units_per_player={per_player} total_units={total}",
                 self.legacy_lines[1],
                 self.legacy_lines[2].replace("created=512", f"created={total}"),
                 self.legacy_lines[3],
                 f"RENDERED_BATTLE_BENCHMARK_GEOMETRY_PASS profile_id={profile_id} roster_contract={roster_contract}",
                 f"RENDERED_BATTLE_BENCHMARK_PLANNER_SUMMARY profile_id={profile_id} roster_contract={roster_contract}",
                 f"RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin frame=1 tick=10 qpc=1000 qpc_frequency=1000 warmup_frames=150 measure_frames=450 logic_target_hz=30 render_pacing=uncapped resolution=1920x1080 windowed=1 fps_source=present_trace profile_id={profile_id} phase_contract={phase_contract}",
                 f"RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_begin frame=151 tick=20 qpc=10000 qpc_frequency=1000 alive0={alive} alive1={alive} attacking={total} profile_id={profile_id} phase_contract={phase_contract}",
                 f"RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop frame=601 tick=30 qpc=14000 qpc_frequency=1000 complete=1 warmup_frames=150 requested_measure_frames=450 actual_measure_frames=450 measured_attack_samples=450 measured_loss_samples=0 alive0={alive} alive1={alive} attacking={total} fps_source=present_trace profile_id={profile_id} phase_contract={phase_contract}"]
        factions = ("FactionAmerica", "FactionChina", "FactionGLA", "FactionAmerica")
        for slot in range(8):
            faction_index = 0 if slot % 4 == 3 else slot % 4
            templates = ",".join(name + ":" + str(count) for name, count in
                                  zip(r1.ROSTER_TEMPLATES[faction_index], template_counts))
            ids = ",".join(str(value) for value in range(slot * per_player + 1,
                                                          (slot + 1) * per_player + 1))
            lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_ROSTER slot={slot} faction={factions[slot % 4]} team={int(slot >= 4)} count={per_player} templates={templates} profile_id={profile_id} roster_contract={roster_contract} unit_type_sequence={expected['unit_type_sequence']} ids={ids}")
        for frame in (151, 301, 451, 601):
            lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_SAMPLE frame={frame} alive0={alive} alive1={alive} health0=100 health1=100 position_sum0=10 position_sum1=10 attacking={total} camera=combat")
        lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_COMPLETE reason=frame_cap terminal_sample=fresh_cap_sample created={total} engine_exit_code=0 diagnostic_exit_code=0 executable_sha256=" + "a" * 64)
        return lines

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, marker=None, position=0):
        lines = list(self.lines)
        if marker is not None: lines.insert(position, marker)
        lines.append(f"RENDERED_BATTLE_DIAGNOSTIC_REPORT_WRITE status=complete buffer_failed=0 records={len(lines)}")
        self.path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def timing_files(self):
        for folder, name in (("present", "present-frame-timing-synthetic.csv"),
                             ("gpu", "gpu-frame-timing-synthetic.csv")):
            target = self.root / folder
            target.mkdir()
            (target / name).write_bytes(b"deliberately invalid: must not be read")

    def test_default_completed_fixture_remains_numerically_eligible(self):
        self.write()
        fixture = r1.diagnostic(self.path, self.ready)
        self.assertEqual(fixture["seed"], 637808953)
        self.assertEqual(fixture["phase"]["warmup_frames"], 150)
        self.assertEqual(fixture["phase"]["measured_frames"], 450)
        self.assertIsNone(fixture["render_admission_metrics"])

    def test_all_six_named_r2_profiles_accept_their_frozen_roster_and_phase_contracts(self):
        self.assertEqual(set(EXPECTED_R2_PROFILE_ROSTERS), set(r1.R2_PROFILE_CONTRACTS))
        for profile_id, expected in EXPECTED_R2_PROFILE_ROSTERS.items():
            with self.subTest(profile=profile_id):
                sequence = expected["unit_type_sequence"]
                self.assertEqual(len(sequence), expected["units_per_player"])
                self.assertEqual(tuple(sequence.count(str(kind)) for kind in range(4)),
                                 expected["template_counts"])
                contract = r1.R2_PROFILE_CONTRACTS[profile_id]
                self.assertEqual(contract["units_per_player"], expected["units_per_player"])
                self.assertEqual(contract["template_counts"], expected["template_counts"])
                self.assertEqual(contract["unit_type_sequence"], sequence)
                self.assertEqual(contract["roster_contract"], expected["roster_contract"])
                self.lines = self.profile_fixture(profile_id)
                self.write()
                fixture = r1.diagnostic(self.path, self.ready)
                self.assertEqual(fixture["profile_contract"]["profile_id"], profile_id)
                self.assertEqual(fixture["profile_contract"]["roster_contract"], expected["roster_contract"])
                self.assertEqual(fixture["profile_contract"]["total_units"], expected["units_per_player"] * 8)
                self.assertEqual(fixture["profile_contract"]["unit_type_sequence"], sequence)
                self.assertEqual(len(fixture["roster"]), 8)

    def test_optional_rigid_draw_metrics_are_separate_from_workload_and_fps_evidence(self):
        self.lines = self.profile_fixture("combined_arms_256")
        warm_index = next(i for i, line in enumerate(self.lines)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin "))
        stop_index = next(i for i, line in enumerate(self.lines)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop "))
        fields = ("captured_draws", "instanced_batches", "instanced_instances",
                  "singleton_ordinary", "unsupported_fallbacks",
                  "ordinary_fallback_draws", "rejected_draws")
        warm_values = (100, 4, 96, 3, 2, 7, 0)
        stop_values = (130, 7, 120, 5, 4, 11, 1)
        def suffix(frame, values, status=0):
            pairs = ["rigid_metrics_schema=" + r1.RIGID_METRICS_SCHEMA,
                     "rigid_metrics_status=" + str(status), "rigid_logic_frame=" + str(frame)]
            pairs.extend("rigid_" + key + "=" + str(value) for key, value in zip(fields, values))
            return " " + " ".join(pairs)
        self.lines[warm_index] += suffix(1, warm_values)
        self.lines[stop_index] += suffix(601, stop_values)
        self.write()
        fixture = r1.diagnostic(self.path, self.ready)
        receipt = fixture["render_admission_metrics"]
        self.assertTrue(receipt["available"])
        self.assertEqual(receipt["logic_frames"], {"warmup_begin": 1, "measurement_stop": 601})
        self.assertEqual(receipt["counter_delta_scope"], {
            "first_logic_frame": 1, "last_logic_frame": 601, "elapsed_logic_frames": 600,
            "warmup_frames": 150, "measured_frames": 450})
        self.assertEqual(receipt["warmup_plus_measurement_delta"], {
            "captured_draws": 30, "instanced_batches": 3, "instanced_instances": 24,
            "singleton_ordinary": 2, "unsupported_fallbacks": 2,
            "ordinary_fallback_draws": 4, "rejected_draws": 1})
        self.assertNotIn("render_admission_metrics", fixture["profile_contract"])
        self.assertIn("admission and route-attempt", receipt["interpretation"])
        self.assertIn("not GPU completion", receipt["interpretation"])

        self.lines[warm_index] = self.lines[warm_index].replace("rigid_metrics_status=0", "rigid_metrics_status=2")
        self.lines[stop_index] = self.lines[stop_index].replace("rigid_metrics_status=0", "rigid_metrics_status=2")
        self.write()
        unavailable = r1.diagnostic(self.path, self.ready)["render_admission_metrics"]
        self.assertFalse(unavailable["available"])
        self.assertIsNone(unavailable["warmup_plus_measurement_delta"])

    def test_legacy_default_rejects_rigid_metrics_without_profile_marker(self):
        self.lines = list(self.legacy_lines)
        warm_index = next(i for i, line in enumerate(self.lines)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin "))
        stop_index = next(i for i, line in enumerate(self.lines)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop "))
        metric_fields = ("captured_draws", "instanced_batches", "instanced_instances",
                         "singleton_ordinary", "unsupported_fallbacks",
                         "ordinary_fallback_draws", "rejected_draws")

        def metric_suffix(frame):
            fields = ["rigid_metrics_schema=" + r1.RIGID_METRICS_SCHEMA,
                      "rigid_metrics_status=0", "rigid_logic_frame=" + str(frame)]
            fields.extend("rigid_" + name + "=0" for name in metric_fields)
            return " " + " ".join(fields)

        # This is a complete, correctly aligned pair on the legacy 512-unit
        # diagnostic. It must fail only because that run has no named R2 profile.
        self.lines[warm_index] += metric_suffix(1)
        self.lines[stop_index] += metric_suffix(601)
        self.write()
        with self.assertRaisesRegex(r1.Reject,
                                    "rigid-draw metrics require an explicit R2 profile"):
            r1.diagnostic(self.path, self.ready)

    def test_rigid_draw_metrics_reject_partial_unknown_misaligned_and_nonmonotonic_receipts(self):
        base = self.profile_fixture("mechanized_256")
        warm_index = next(i for i, line in enumerate(base)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=warmup_begin "))
        stop_index = next(i for i, line in enumerate(base)
                          if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_stop "))
        begin_index = next(i for i, line in enumerate(base)
                           if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_begin "))
        def with_metrics():
            lines = list(base)
            values = "rigid_metrics_schema=" + r1.RIGID_METRICS_SCHEMA + " rigid_metrics_status=0 rigid_logic_frame={frame} rigid_captured_draws={draws} " \
                "rigid_instanced_batches=4 rigid_instanced_instances=80 rigid_singleton_ordinary=0 " \
                "rigid_unsupported_fallbacks=0 rigid_ordinary_fallback_draws=0 rigid_rejected_draws=0"
            lines[warm_index] += " " + values.format(frame=1, draws=10)
            lines[stop_index] += " " + values.format(frame=601, draws=20)
            return lines
        mutations = (
            ("missing stop snapshot", lambda lines: lines.__setitem__(stop_index, lines[stop_index].split(" rigid_metrics_schema=")[0])),
            ("partial fields", lambda lines: lines.__setitem__(warm_index, lines[warm_index].replace(" rigid_rejected_draws=0", ""))),
            ("duplicate field", lambda lines: lines.__setitem__(warm_index, lines[warm_index] + " rigid_captured_draws=10")),
            ("unknown field", lambda lines: lines.__setitem__(stop_index, lines[stop_index] + " rigid_gpu_completions=2")),
            ("unknown schema", lambda lines: lines.__setitem__(stop_index, lines[stop_index].replace(r1.RIGID_METRICS_SCHEMA, "ggc.r2.rendered-battle-rigid-draw-metrics.v2"))),
            ("wrong logic frame", lambda lines: lines.__setitem__(warm_index, lines[warm_index].replace("rigid_logic_frame=1", "rigid_logic_frame=2"))),
            ("negative counter", lambda lines: lines.__setitem__(stop_index, lines[stop_index].replace("rigid_rejected_draws=0", "rigid_rejected_draws=-1"))),
            ("decreasing counter", lambda lines: lines.__setitem__(stop_index, lines[stop_index].replace("rigid_captured_draws=20", "rigid_captured_draws=9"))),
            ("unknown status", lambda lines: lines.__setitem__(stop_index, lines[stop_index].replace("rigid_metrics_status=0", "rigid_metrics_status=9"))),
            ("metrics on middle phase", lambda lines: lines.__setitem__(begin_index, lines[begin_index] + " rigid_metrics_status=0")),
        )
        for label, mutate in mutations:
            with self.subTest(case=label):
                self.lines = with_metrics()
                mutate(self.lines)
                self.write()
                with self.assertRaises(r1.Reject):
                    r1.diagnostic(self.path, self.ready)

    def test_r2_profile_rejects_unknown_schema_density_and_partial_identity(self):
        base = self.profile_fixture("combined_arms_256")
        profile_index = next(i for i, line in enumerate(base)
                             if line.startswith("RENDERED_BATTLE_BENCHMARK_PROFILE "))
        mutations = (
            lambda line: line.replace("id=combined_arms_256", "id=unknown_profile"),
            lambda line: line.replace(r1.R2_PROFILE_SCHEMA, "ggc.r2.rendered-battle-profile.v2"),
            lambda line: line.replace("total_units=256", "total_units=512"),
            lambda line: line.replace(" phase_contract=" + r1.R2_PHASE_CONTRACT, ""),
            lambda line: line + " unexpected=1",
        )
        for mutate in mutations:
            with self.subTest(marker=mutate(base[profile_index])):
                self.lines = list(base)
                self.lines[profile_index] = mutate(self.lines[profile_index])
                self.write()
                with self.assertRaises(r1.Reject):
                    r1.diagnostic(self.path, self.ready)

    def test_r2_roster_rejects_wrong_template_mix_or_unit_type_sequence(self):
        base = self.profile_fixture("mechanized_256")
        roster_index = next(i for i, line in enumerate(base)
                            if line.startswith("RENDERED_BATTLE_DIAGNOSTIC_ROSTER "))
        for old, new in (("AmericaTankCrusader:16", "AmericaTankCrusader:15"),
                         ("unit_type_sequence=" + r1.R2_PROFILE_CONTRACTS["mechanized_256"]["unit_type_sequence"],
                          "unit_type_sequence=" + "1" * 16 + "0" * 16)):
            with self.subTest(replacement=new):
                self.lines = list(base)
                self.lines[roster_index] = self.lines[roster_index].replace(old, new)
                self.write()
                with self.assertRaises(r1.Reject):
                    r1.diagnostic(self.path, self.ready)

    def test_r2_identity_must_repeat_on_each_roster_and_measured_phase(self):
        base = self.profile_fixture("infantry_line_512")
        roster_index = next(i for i, line in enumerate(base)
                            if line.startswith("RENDERED_BATTLE_DIAGNOSTIC_ROSTER "))
        phase_index = next(i for i, line in enumerate(base)
                           if line.startswith("RENDERED_BATTLE_BENCHMARK_PHASE phase=measurement_begin "))
        changes = (
            (roster_index, "roster_contract=ggc.r2.rendered-battle.roster.infantry-line-512.v1",
             "roster_contract=ggc.r2.rendered-battle.roster.infantry-line-256.v1"),
            (roster_index, "profile_id=infantry_line_512", "profile_id=mechanized_512"),
            (phase_index, "phase_contract=" + r1.R2_PHASE_CONTRACT,
             "phase_contract=ggc.r2.rendered-battle.phase.other.v1"),
            (phase_index, "profile_id=infantry_line_512", "profile_id=combined_arms_512"),
        )
        for index, old, new in changes:
            with self.subTest(old=old):
                self.lines = list(base)
                self.lines[index] = self.lines[index].replace(old, new)
                self.write()
                with self.assertRaises(r1.Reject):
                    r1.diagnostic(self.path, self.ready)

    def test_stripped_r2_profile_marker_cannot_fall_back_to_legacy_512(self):
        self.lines = self.profile_fixture("combined_arms_512")
        self.lines = [line for line in self.lines
                      if not line.startswith("RENDERED_BATTLE_BENCHMARK_PROFILE ")]
        self.write()
        with self.assertRaisesRegex(r1.Reject, "profile identity fields appear without"):
            r1.diagnostic(self.path, self.ready)

    def test_diagnostic_requires_literal_seed_even_when_requests_match(self):
        original = self.lines[0]
        for seed in ("42", "637808954"):
            self.lines[0] = original.replace("seed=637808953", "seed=" + seed)
            self.ready["arguments"][1] = seed
            self.write()
            with self.assertRaisesRegex(r1.Reject, "required requested fixture seed"):
                r1.diagnostic(self.path, self.ready)

    def test_diagnostic_rejects_ready_seed_mismatch_or_unbound_seed_token(self):
        for arguments in (["-runRenderedBattleBenchmark", "42"],
                          ["-runRenderedBattleBenchmark", "42", "637808953"],
                          ["637808953", "-runRenderedBattleBenchmark"],
                          ["-runRenderedBattleBenchmark", "0637808953"]):
            self.ready["arguments"] = arguments
            self.write()
            with self.assertRaises(r1.Reject): r1.diagnostic(self.path, self.ready)

    def test_start_seed_must_exist_and_match_begin_and_request(self):
        original = self.lines[1]
        for value in (None, "42", "637808954", "637808953.0", "0637808953"):
            self.lines[1] = original.replace("seed=637808953 ", "" if value is None else "seed=" + value + " ")
            self.write()
            with self.assertRaisesRegex(r1.Reject, "START seed"):
                r1.diagnostic(self.path, self.ready)

    def test_requested_seed_rejects_duplicate_or_nonstrict_arguments(self):
        for arguments in (["-runRenderedBattleBenchmark", "637808953", "-runRenderedBattleBenchmark", "637808953"],
                          ["-runRenderedBattleBenchmark", "637808953", "-runRenderedBattleDiagnostic", "637808953"],
                          ["-runRenderedBattleBenchmark", 637808953],
                          ["-runRenderedBattleBenchmark", "637808953.0"]):
            with self.assertRaises(r1.Reject): r1.requested_fixture_seed({"arguments": arguments})

    def test_start_map_requires_fixture_label_or_exact_native_relative_path(self):
        original = self.lines[1]
        for map_name in ("Fortress_Avalanche", '"Maps\\Fortress Avalanche\\Fortress Avalanche.map"',
                         '"maps/fortress avalanche/fortress avalanche.map"'):
            self.lines[1] = original.replace("map=Fortress_Avalanche", "map=" + map_name)
            self.write()
            r1.diagnostic(self.path, self.ready)
        for map_name in ("Other_Map", '"Maps\\Other\\Fortress Avalanche.map"',
                         '"C:\\Maps\\Fortress Avalanche\\Fortress Avalanche.map"', ""):
            self.lines[1] = original.replace("map=Fortress_Avalanche", "map=" + map_name)
            self.write()
            with self.assertRaisesRegex(r1.Reject, "START map differs"):
                r1.diagnostic(self.path, self.ready)

    def test_all_runs_with_same_wrong_seed_cannot_qualify_comparison(self):
        self.lines[0] = self.lines[0].replace("seed=637808953", "seed=42")
        self.ready["arguments"][1] = "42"
        self.write()
        self.timing_files()
        runs = [self.root, self.root / "b2", self.root / "c1", self.root / "c2"]
        for root in runs[1:]:
            (root / "profile").mkdir(parents=True)
            (root / "profile" / self.path.name).write_bytes(self.path.read_bytes())
            (root / "present").mkdir()
            (root / "present" / "present-frame-timing-synthetic.csv").write_bytes(b"must not be read")
        def captured(root, proof):
            return dict(self.ready, test_options=str(root / "profile" / "Options.ini")), {}
        with patch.object(r1, "controller", side_effect=captured), patch.object(r1, "present") as present:
            result = r1.comparison([str(root) for root in runs[:2]], [str(root) for root in runs[2:]], 500, 500)
            present.assert_not_called()
        self.assertFalse(result["data_comparable"])
        self.assertEqual(result["runs"], [])
        self.assertIn("required requested fixture seed", result["rejections"][0]["reasons"][0])
        self.assertNotIn("mean_fps_delta", result)

    def test_all_runs_with_same_wrong_template_mix_cannot_qualify_comparison(self):
        self.lines = [line.replace(":24", ":23") if line.startswith("RENDERED_BATTLE_DIAGNOSTIC_ROSTER ") else line
                      for line in self.lines]
        self.write()
        self.timing_files()
        runs = [self.root, self.root / "b2", self.root / "c1", self.root / "c2"]
        for root in runs[1:]:
            (root / "profile").mkdir(parents=True)
            (root / "profile" / self.path.name).write_bytes(self.path.read_bytes())
            (root / "present").mkdir()
            (root / "present" / "present-frame-timing-synthetic.csv").write_bytes(b"must not be read")
        def captured(root, proof):
            return dict(self.ready, test_options=str(root / "profile" / "Options.ini")), {}
        with patch.object(r1, "controller", side_effect=captured), patch.object(r1, "present") as present:
            result = r1.comparison([str(root) for root in runs[:2]], [str(root) for root in runs[2:]], 500, 500)
            present.assert_not_called()
        self.assertFalse(result["data_comparable"])
        self.assertEqual(result["runs"], [])
        self.assertIn("roster template mix", result["rejections"][0]["reasons"][0])
        self.assertNotIn("mean_fps_delta", result)

    def test_completed_visual_fixture_rejects_any_marker_fields_or_position(self):
        for marker in ("RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=0 wall_cap_ms=120000",
                       "RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=1 wall_cap_ms=1",
                       "RENDERED_BATTLE_VISUAL_CAPTURE_ONLY",
                       "prefix RENDERED_BATTLE_VISUAL_CAPTURE_ONLY malformed"):
            for position in (0, len(self.lines)):
                self.write(marker, position)
                with self.assertRaisesRegex(r1.Reject, "visual-only capture"):
                    r1.diagnostic(self.path, self.ready)

    def test_analyze_visual_capture_refuses_before_present_or_gpu_read(self):
        self.write("RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=0 wall_cap_ms=120000")
        self.timing_files()
        original_open = Path.open
        def guarded_open(path, *args, **kwargs):
            if path.suffix == ".csv": self.fail("visual capture opened timing CSV")
            return original_open(path, *args, **kwargs)
        with patch.object(r1, "controller", return_value=(self.ready, {})), patch.object(Path, "open", guarded_open), patch.object(r1, "present") as present, patch.object(r1, "gpu_evidence") as gpu:
            with self.assertRaisesRegex(r1.Reject, "visual-only capture"):
                r1.analyze(self.root)
            present.assert_not_called()
            gpu.assert_not_called()

    def test_comparison_visual_capture_returns_rejection_without_numbers(self):
        self.write("RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=0 wall_cap_ms=120000", len(self.lines))
        self.timing_files()
        with patch.object(r1, "controller", return_value=(self.ready, {})), patch.object(r1, "present") as present, patch.object(r1, "gpu_evidence") as gpu:
            result = r1.comparison([str(self.root), str(self.root / "b2")],
                                   [str(self.root / "c1"), str(self.root / "c2")], 500, 500)
            present.assert_not_called()
            gpu.assert_not_called()
        self.assertFalse(result["data_comparable"])
        self.assertEqual(result["runs"], [])
        self.assertIn("visual-only capture", result["rejections"][0]["reasons"][0])
        for field in ("baseline", "candidate", "mean_fps_delta", "mean_fps_delta_percent"):
            self.assertNotIn(field, result)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".comparison-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_every_compared_run_requires_controller_sha256(self):
        for missing_all in (False, True):
            for value in (None, "", "invalid", 123, False):
                reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                           comparison_run("candidate", 1), comparison_run("candidate", 2)]
                for report in (reports if missing_all else reports[-1:]):
                    report["identity"]["controller_script_sha256"] = value
                with patch.object(r1, "analyze", side_effect=reports):
                    result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
                self.assertFalse(result["data_comparable"])
                self.assertTrue(any("controller_script_sha256 cannot be compared" in reason
                                    for rejection in result["rejections"] for reason in rejection["reasons"]))
                self.assertNotIn("mean_fps_delta", result)

    def test_matching_controller_hashes_compare_case_insensitively(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        reports[-1]["identity"]["controller_script_sha256"] = "A" * 64
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertTrue(result["data_comparable"])

    def compare_paths(self, baseline, candidate):
        with patch.object(r1, "analyze") as mocked:
            result = r1.comparison(baseline, candidate, 500, 500)
        mocked.assert_not_called()
        self.assertFalse(result["data_comparable"])
        return result

    def test_duplicate_baseline_path_rejected_before_analysis(self):
        baseline = str(self.root / "baseline")
        result = self.compare_paths([baseline, baseline.swapcase()],
                                    [str(self.root / "candidate-1"), str(self.root / "candidate-2")])
        self.assertIn("duplicate baseline run path", result["rejections"][0]["reasons"])

    def test_duplicate_candidate_path_rejected_before_analysis(self):
        candidate = str(self.root / "candidate")
        result = self.compare_paths([str(self.root / "baseline-1"), str(self.root / "baseline-2")],
                                    [candidate, candidate.swapcase()])
        self.assertIn("duplicate candidate run path", result["rejections"][0]["reasons"])

    def test_baseline_candidate_path_overlap_rejected_before_analysis(self):
        shared = str(self.root / "shared-run")
        result = self.compare_paths([str(self.root / "baseline"), shared],
                                    [shared.swapcase(), str(self.root / "candidate")])
        self.assertIn("baseline/candidate run path overlap", result["rejections"][0]["reasons"])

    def test_mixed_window_exposure_rejected_even_when_all_runs_are_mixed(self):
        reports = [comparison_run("baseline", 1, mode="mixed"),
                   comparison_run("baseline", 2, mode="mixed"),
                   comparison_run("candidate", 1, mode="mixed"),
                   comparison_run("candidate", 2, mode="mixed")]
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("mixed or unknown" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_uniform_background_is_comparable_but_not_stage_accepted(self):
        reports = [comparison_run("baseline", 1, mode="background", fps=15.0),
                   comparison_run("baseline", 2, mode="background", fps=15.2),
                   comparison_run("candidate", 1, mode="background", fps=16.0),
                   comparison_run("candidate", 2, mode="background", fps=16.2)]
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertTrue(result["data_comparable"])
        self.assertTrue(result["accepted"])
        self.assertTrue(result["accepted_deprecated"])
        self.assertFalse(result["stage_accepted"])
        self.assertEqual(result["stage_status"], "not_evaluated")
        self.assertGreater(result["mean_fps_delta"], 0)

    def test_comparison_rejects_mixed_named_profile_and_legacy_default_identity(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        profile = {"schema": r1.R2_PROFILE_SCHEMA, "profile_id": "combined_arms_512",
                   "roster_contract": r1.R2_PROFILE_CONTRACTS["combined_arms_512"]["roster_contract"],
                   "phase_contract": r1.R2_PHASE_CONTRACT, "units_per_player": 64,
                   "total_units": 512, "template_counts": (8, 8, 24, 24),
                   "unit_type_sequence": r1.R2_PROFILE_CONTRACTS["combined_arms_512"]["unit_type_sequence"]}
        for report in reports[:3]:
            report["workload_identity"]["profile_contract"] = profile
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("fixture profile_contract mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_comparison_rejects_different_captured_affinity(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        reports[-1]["identity"]["captured_affinity"]["game"] = "0x3"
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("captured_affinity mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_comparison_rejects_repeated_present_run_identity_at_distinct_paths(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        reports[1]["present"]["owner"] = dict(reports[0]["present"]["owner"])
        reports[1]["phase"] = dict(reports[0]["phase"])
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["copied-a", "copied-b"], ["candidate-a", "candidate-b"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("duplicate actual game-run identity" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_non_null_controller_hashes_must_match_in_minimal_mode(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        for report in reports:
            report["identity"]["controller_script_sha256"] = "a" * 64
        reports[-1]["identity"]["controller_script_sha256"] = "b" * 64
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("controller_script_sha256 mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_present_gpu_comparison_rejects_adapter_identity_mismatch(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        adapter = {"vendor_id": 4318, "device_id": 7956, "luid_low": 88384,
                   "luid_high": 0, "feature_level": 45056, "software": False,
                   "debug_layer": False}
        for report in reports:
            report["identity"].update(telemetry="presentGpu",
                                      controller_script_sha256="a" * 64,
                                      gpu_adapter_identity=dict(adapter),
                                      effective_quality_contract={"width": 1920,
                                                                  "height": 1080,
                                                                  "msaa_samples": 8,
                                                                  "resolve_applied": True})
        reports[-1]["identity"]["gpu_adapter_identity"]["luid_low"] = 99999
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertFalse(result["stage_accepted"])
        self.assertEqual(result["stage_status"], "not_evaluated")
        self.assertTrue(any("gpu_adapter_identity mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))

    def test_present_gpu_comparison_allows_different_timestamp_query_coverage(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        adapter = {"vendor_id": 4318, "device_id": 7956, "luid_low": 88384,
                   "luid_high": 0, "feature_level": 45056, "software": False,
                   "debug_layer": False}
        for index, report in enumerate(reports):
            report["identity"].update(telemetry="presentGpu",
                                      controller_script_sha256="a" * 64,
                                      gpu_adapter_identity=dict(adapter),
                                      effective_quality_contract={"width": 1920,
                                                                  "height": 1080,
                                                                  "msaa_samples": 8,
                                                                  "resolve_applied": True})
            report["effective_quality_evidence"] = {
                "measurement_phase": {"timestamp_coverage_fraction": 0.25 if index < 2 else 0.75}}
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertTrue(result["data_comparable"])
        self.assertFalse(result["stage_accepted"])

    def test_present_gpu_comparison_rejects_debug_layer_mismatch(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        adapter = {"vendor_id": 4318, "device_id": 7956, "luid_low": 88384,
                   "luid_high": 0, "feature_level": 45056, "software": False,
                   "debug_layer": False}
        for report in reports:
            report["identity"].update(telemetry="presentGpu",
                                      controller_script_sha256="a" * 64,
                                      gpu_adapter_identity=dict(adapter),
                                      effective_quality_contract={"width": 1920,
                                                                  "height": 1080,
                                                                  "msaa_samples": 8,
                                                                  "resolve_applied": True})
        reports[-1]["identity"]["gpu_adapter_identity"]["debug_layer"] = True
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("gpu_adapter_identity mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))


class LauncherConfigurationTests(unittest.TestCase):
    CONTENT = "RUN = . generalszh.exe -simulationMode parallel -workerPolicy auto"
    BYTES = (CONTENT + "\r\n").encode("ascii")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".launcher-config-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def ready_with_configuration(self, name):
        runtime = self.root / name
        runtime.mkdir()
        path = runtime / "launcher.lcf"
        path.write_bytes(self.BYTES)
        import hashlib
        return {"runtime": str(runtime),
                "launcher_configuration": {"path": str(path),
                                           "sha256": hashlib.sha256(self.BYTES).hexdigest().upper(),
                                           "content": self.CONTENT}}

    def test_different_runtime_paths_with_identical_real_lcf_bytes_compare_and_retain_provenance(self):
        reports = []
        paths = []
        for role in ("baseline", "candidate"):
            for number in (1, 2):
                ready = self.ready_with_configuration(f"{role}-{number}-runtime")
                verified = r1.verified_launcher_configuration(ready)
                paths.append(verified["path"])
                report = comparison_run(role, number)
                report["identity"]["launcher_configuration"] = verified
                reports.append(report)
        self.assertEqual(len({path.casefold() for path in paths}), 4)
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["baseline-1", "baseline-2"],
                                   ["candidate-1", "candidate-2"], 500, 500)
        self.assertTrue(result["data_comparable"], result.get("rejections"))
        self.assertEqual([r["identity"]["launcher_configuration"]["path"]
                          for r in result["runs"]], paths)

    def test_changed_lcf_bytes_after_receipt_are_rejected(self):
        ready = self.ready_with_configuration("changed-bytes")
        (Path(ready["runtime"]) / "launcher.lcf").write_bytes(b"RUN = . generalszh.exe\r\n")
        with self.assertRaisesRegex(r1.Reject, "SHA-256 differs"):
            r1.verified_launcher_configuration(ready)

    def test_wrong_declared_lcf_hash_is_rejected(self):
        ready = self.ready_with_configuration("wrong-hash")
        ready["launcher_configuration"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(r1.Reject, "SHA-256 differs"):
            r1.verified_launcher_configuration(ready)

    def test_wrong_declared_lcf_content_is_rejected(self):
        ready = self.ready_with_configuration("wrong-content")
        ready["launcher_configuration"]["content"] = "RUN = . generalszh.exe"
        with self.assertRaisesRegex(r1.Reject, "content differs"):
            r1.verified_launcher_configuration(ready)

    def test_controller_rejects_missing_launcher_configuration(self):
        run = self.root / "missing-controller-configuration"
        run.mkdir()
        runtime = self.root / "missing-controller-runtime"
        runtime.mkdir()
        shared = {"schema": "ggc.bounded-native-controller.v1",
                  "run": str(run), "runtime": str(runtime)}
        ready = dict(shared, status="ready")
        exited = dict(shared, status="normal_exit",
                      exit={"normal_exit": True, "forced_termination": False, "error": None})
        (run / "ready.json").write_text(json.dumps(ready), encoding="utf-8")
        (run / "exit.json").write_text(json.dumps(exited), encoding="utf-8")
        with self.assertRaisesRegex(r1.Reject, "launcher_configuration must be an object"):
            r1.controller(run)

    def test_missing_nonfile_and_mismatched_lcf_paths_are_rejected(self):
        ready = self.ready_with_configuration("path-validation")
        missing = dict(ready)
        del missing["launcher_configuration"]
        with self.assertRaisesRegex(r1.Reject, "launcher_configuration must be an object"):
            r1.verified_launcher_configuration(missing)
        nonfile = {"runtime": ready["runtime"],
                   "launcher_configuration": dict(ready["launcher_configuration"])}
        nonfile["launcher_configuration"]["path"] = ready["runtime"]
        with self.assertRaises(r1.Reject):
            r1.verified_launcher_configuration(nonfile)
        other = self.root / "other-runtime"
        other.mkdir()
        other_path = other / "launcher.lcf"
        other_path.write_bytes(self.BYTES)
        mismatched = {"runtime": ready["runtime"],
                      "launcher_configuration": dict(ready["launcher_configuration"])}
        mismatched["launcher_configuration"]["path"] = str(other_path)
        with self.assertRaisesRegex(r1.Reject, "does not identify runtime launcher.lcf"):
            r1.verified_launcher_configuration(mismatched)


class ControllerIntegerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".controller-integers-")
        self.root = Path(self.tmp.name)
        runtime = self.root / "runtime"
        runtime.mkdir()
        lcf = runtime / "launcher.lcf"
        lcf.write_bytes(b"RUN = . generalszh.exe\r\n")
        game_path = self.root / "generalszh.exe"
        launcher_path = runtime / "launcher.exe"
        options_path = self.root / "Options.ini"
        game_path.write_bytes(b"synthetic game image")
        launcher_path.write_bytes(b"synthetic launcher image")
        options_path.write_bytes(b"synthetic options")
        game = {"pid": 77, "started_utc": "game-start", "executable": str(game_path),
                "sha256": r1.digest(game_path), "parent_pid": 20, "affinity_hex": "0xFFF"}
        launcher = {"pid": 20, "started_utc": "launcher-start",
                    "executable": str(launcher_path), "sha256": r1.digest(launcher_path),
                    "parent_pid": 10, "affinity_hex": "0xFFF"}
        self.ready = {
            "schema": "ggc.bounded-native-controller.v1", "status": "ready",
            "run": str(self.root), "runtime": str(runtime), "scene": "Benchmark",
            "telemetry": "minimalPresent", "telemetry_channels": None,
            "controller_script_sha256": None, "shadow_experiment": None,
            "source_commit": "a" * 40, "snapshot_identity": "b" * 64,
            "source_options_sha256": "c" * 64,
            "test_options_sha256": r1.digest(options_path),
            "expected_executable_sha256": game["sha256"],
            "expected_launcher_sha256": launcher["sha256"],
            "requested_settings": {"Resolution": "1920 1080", "AntiAliasing": "8",
                                   "TextureFilter": "Anisotropic", "AnisotropyLevel": "16"},
            "arguments": ["-runRenderedBattleBenchmark", "637808953"],
            "launcher_configuration": {"path": str(lcf), "sha256": r1.digest(lcf),
                                        "content": "RUN = . generalszh.exe"},
            "affinity": {"mask_hex": "0xFFF"}, "game": game, "launcher": launcher,
            "test_options": str(options_path),
        }
        self.exit = {
            "schema": "ggc.bounded-native-controller.v1", "status": "normal_exit",
            "exit": {"normal_exit": True, "forced_termination": False, "error": None,
                     "processes": [{"pid": 77, "exited": True, "exit_code": 0},
                                   {"pid": 20, "exited": True, "exit_code": 0}]},
            **{key: json.loads(json.dumps(self.ready[key])) for key in (
                "run", "runtime", "scene", "telemetry", "telemetry_channels",
                "controller_script_sha256", "shadow_experiment", "source_commit",
                "snapshot_identity", "source_options_sha256", "test_options_sha256",
                "expected_executable_sha256", "expected_launcher_sha256",
                "requested_settings", "arguments", "launcher_configuration", "affinity")},
            "game": json.loads(json.dumps(game)),
            "launcher": json.loads(json.dumps(launcher)),
        }

    def tearDown(self):
        self.tmp.cleanup()

    def run_controller(self, ready, exit_receipt):
        (self.root / "ready.json").write_text(json.dumps(ready), encoding="utf-8")
        (self.root / "exit.json").write_text(json.dumps(exit_receipt), encoding="utf-8")
        return r1.controller(self.root)

    def test_controller_rejects_fractional_exit_codes_and_pids(self):
        self.assertEqual(self.run_controller(self.ready, self.exit)[1]["game_sha256"],
                         self.ready["game"]["sha256"].upper())
        for field in ("exit_code", "integral_float_exit_code", "exit_pid", "ready_pid"):
            with self.subTest(field=field):
                ready = json.loads(json.dumps(self.ready))
                exit_receipt = json.loads(json.dumps(self.exit))
                if field in ("exit_code", "integral_float_exit_code"):
                    exit_receipt["exit"]["processes"][0]["exit_code"] = (
                        0.0 if field == "integral_float_exit_code" else 0.9)
                elif field == "exit_pid":
                    exit_receipt["exit"]["processes"][0]["pid"] = 77.9
                else:
                    ready["game"]["pid"] = exit_receipt["game"]["pid"] = 77.9
                with self.assertRaisesRegex(r1.Reject, "must be an integer"):
                    self.run_controller(ready, exit_receipt)


    def test_controller_rejects_json_string_integer_fields(self):
        for field in ("exit_code", "exit_pid", "game_pid", "launcher_pid", "game_parent", "launcher_parent", "exit_game_pid"):
            ready = json.loads(json.dumps(self.ready))
            receipt = json.loads(json.dumps(self.exit))
            if field in ("exit_code", "exit_pid"):
                key = "exit_code" if field == "exit_code" else "pid"
                receipt["exit"]["processes"][0][key] = str(receipt["exit"]["processes"][0][key])
            elif field == "exit_game_pid": receipt["game"]["pid"] = "77"
            else:
                label = "launcher" if field.startswith("launcher") else "game"
                key = "parent_pid" if field.endswith("parent") else "pid"
                ready[label][key] = receipt[label][key] = str(ready[label][key])
            with self.assertRaisesRegex(r1.Reject, "integer JSON value"):
                self.run_controller(ready, receipt)

    def test_controller_rejects_coherent_nonpositive_process_identities(self):
        for field in ("launcher_chain", "game_pid", "launcher_parent"):
            for value in (0, -1):
                with self.subTest(field=field, value=value):
                    ready = json.loads(json.dumps(self.ready))
                    receipt = json.loads(json.dumps(self.exit))
                    if field == "launcher_chain":
                        for capture in (ready, receipt):
                            capture["launcher"]["pid"] = value
                            capture["game"]["parent_pid"] = value
                        receipt["exit"]["processes"][1]["pid"] = value
                    elif field == "game_pid":
                        ready["game"]["pid"] = receipt["game"]["pid"] = value
                        receipt["exit"]["processes"][0]["pid"] = value
                    else:
                        ready["launcher"]["parent_pid"] = receipt["launcher"]["parent_pid"] = value
                    with self.assertRaisesRegex(r1.Reject, "must be positive"):
                        self.run_controller(ready, receipt)


    def test_captured_controller_artifact_is_bound_when_path_is_available(self):
        path = self.root / "controller.ps1"
        path.write_bytes(b"synthetic captured controller")
        ready = json.loads(json.dumps(self.ready))
        receipt = json.loads(json.dumps(self.exit))
        for capture in (ready, receipt):
            capture["controller_script_sha256"] = r1.digest(path)
            capture["controller_script_path"] = str(path)
            capture["shadow_experiment"] = ShadowExperimentTests.config()
        self.assertEqual(self.run_controller(ready, receipt)[1]["controller_script_path"], str(path))
        path.write_bytes(b"changed controller bytes")
        with self.assertRaisesRegex(r1.Reject, "controller artifact SHA-256"):
            self.run_controller(ready, receipt)
        path.unlink()
        with self.assertRaisesRegex(r1.Reject, "controller artifact is missing"):
            self.run_controller(ready, receipt)
        receipt["controller_script_path"] = str(self.root / "other.ps1")
        with self.assertRaisesRegex(r1.Reject, "ready/exit mismatch: controller_script_path"):
            self.run_controller(ready, receipt)


class QualificationExclusionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".exclusion-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_with_marker(self, name, contents):
        run = self.root / name
        run.mkdir()
        (run / "qualification-exclusion.json").write_text(contents, encoding="utf-8")
        return run

    def comparison_groups(self, excluded, role):
        baseline = [str(self.root / "baseline-1"), str(self.root / "baseline-2")]
        candidate = [str(self.root / "candidate-1"), str(self.root / "candidate-2")]
        group = baseline if role == "baseline" else candidate
        group[0] = str(excluded)
        return baseline, candidate

    @staticmethod
    def valid_marker(reason):
        return json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": True,
                           "reason": reason, "created_utc": "2026-10-04T09:51:16Z"})

    def test_analyze_rejects_valid_marker_before_reading_receipts_and_preserves_reason(self):
        reason = "Operator parameter errors caused startup helper error loops."
        run = self.run_with_marker("excluded-analyze", self.valid_marker(reason))
        with patch.object(r1, "controller") as controller:
            with self.assertRaises(r1.Reject) as caught:
                r1.analyze(run)
        controller.assert_not_called()
        self.assertIn(reason, str(caught.exception))

    def test_analyze_rejects_malformed_and_invalid_marker_values(self):
        invalid_markers = (
            "{",
            '{"schema":"ggc.performance-exclusion.v1","excluded":false,"excluded":true,"reason":"reason"}',
            json.dumps(["not", "an", "object"]),
            json.dumps({"schema": "wrong", "excluded": True, "reason": "reason"}),
            json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": False,
                        "reason": "reason"}),
            json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": 1,
                        "reason": "reason"}),
            json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": True,
                        "reason": "  "}),
        )
        for index, contents in enumerate(invalid_markers):
            with self.subTest(index=index):
                run = self.run_with_marker("invalid-analyze-" + str(index), contents)
                with patch.object(r1, "controller") as controller:
                    with self.assertRaises(r1.Reject):
                        r1.analyze(run)
                controller.assert_not_called()

    def test_comparison_rejects_valid_marker_in_either_group_and_preserves_reason(self):
        reason = "Exclude this operator-tainted run from matched FPS comparisons."
        for role in ("baseline", "candidate"):
            with self.subTest(role=role):
                run = self.run_with_marker("excluded-" + role, self.valid_marker(reason))
                baseline, candidate = self.comparison_groups(run, role)
                with patch.object(r1, "analyze") as analyze:
                    result = r1.comparison(baseline, candidate, 500, 500)
                analyze.assert_not_called()
                self.assertFalse(result["data_comparable"])
                self.assertFalse(result["accepted"])
                self.assertFalse(result["stage_accepted"])
                self.assertEqual(result["stage_status"], "not_evaluated")
                rejection = next(x for x in result["rejections"] if x["run"] == str(run))
                self.assertEqual(rejection["exclusion_reason"], reason)

    def test_comparison_rejects_malformed_false_and_invalid_markers(self):
        invalid_markers = (
            "{",
            '{"schema":"ggc.performance-exclusion.v1","excluded":false,"excluded":true,"reason":"reason"}',
            json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": False,
                        "reason": "reason"}),
            json.dumps({"schema": "ggc.performance-exclusion.v1", "excluded": "true",
                        "reason": "reason"}),
        )
        for index, contents in enumerate(invalid_markers):
            with self.subTest(index=index):
                run = self.run_with_marker("invalid-compare-" + str(index), contents)
                baseline, candidate = self.comparison_groups(run, "baseline")
                with patch.object(r1, "analyze") as analyze:
                    result = r1.comparison(baseline, candidate, 500, 500)
                analyze.assert_not_called()
                self.assertFalse(result["data_comparable"])
                self.assertTrue(any("qualification exclusion marker" in reason.lower()
                                    for rejection in result["rejections"]
                                    for reason in rejection["reasons"]))

    def test_temporary_operator_marker_is_rejected_by_analyze_and_compare(self):
        reason = "Synthetic operator-tainted run must be excluded."
        run = self.run_with_marker("excluded-run", self.valid_marker(reason))
        with self.assertRaises(r1.Reject) as caught:
            r1.analyze(run)
        self.assertIn(reason, str(caught.exception))

        baseline, candidate = self.comparison_groups(run, "baseline")
        valid_reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                         comparison_run("candidate", 1), comparison_run("candidate", 2)]
        self.assertTrue(all(report["qualified"] for report in valid_reports))
        with patch.object(r1, "analyze", side_effect=valid_reports) as mocked_analyze:
            result = r1.comparison(baseline, candidate, 500, 500)
        mocked_analyze.assert_not_called()
        self.assertFalse(result["data_comparable"])
        rejection = next(x for x in result["rejections"] if x["run"] == str(run))
        self.assertEqual(rejection["exclusion_reason"], reason)


class AffinityIdentityTests(unittest.TestCase):
    @staticmethod
    def identities(game_affinity="0xFFF", launcher_affinity="0xFFF"):
        game = {"pid": 77, "started_utc": "game-start", "executable": "game.exe",
                "sha256": "a" * 64, "parent_pid": 20, "affinity_hex": game_affinity}
        launcher = {"pid": 20, "started_utc": "launcher-start", "executable": "launcher.exe",
                    "sha256": "b" * 64, "parent_pid": 10, "affinity_hex": launcher_affinity}
        return game, launcher

    def test_game_and_launcher_captured_masks_must_match_declared_mask(self):
        game, launcher = self.identities()
        result = r1.captured_affinity_identity(game, launcher, dict(game), dict(launcher),
                                               {"mask_hex": "0xfff"})
        self.assertEqual(result, {"game": "0xFFF", "launcher": "0xFFF"})
        game["affinity_hex"] = "0x3"
        with self.assertRaisesRegex(r1.Reject, "game captured process affinity differs"):
            r1.captured_affinity_identity(game, launcher, dict(game), dict(launcher),
                                          {"mask_hex": "0xFFF"})

    def test_launcher_captured_mask_must_match_declared_mask(self):
        game, launcher = self.identities(launcher_affinity="0x3")
        with self.assertRaisesRegex(r1.Reject, "launcher captured process affinity differs"):
            r1.captured_affinity_identity(game, launcher, dict(game), dict(launcher),
                                          {"mask_hex": "0xFFF"})

    def test_ready_exit_affinity_change_is_rejected(self):
        game, launcher = self.identities()
        exit_game = dict(game, affinity_hex="0x3")
        with self.assertRaisesRegex(r1.Reject, "game ready/exit identity mismatch"):
            r1.captured_affinity_identity(game, launcher, exit_game, dict(launcher),
                                          {"mask_hex": "0xFFF"})

    def test_comparison_rejects_shadow_experiment_setting_mismatch(self):
        reports = [comparison_run("baseline", 1), comparison_run("baseline", 2),
                   comparison_run("candidate", 1), comparison_run("candidate", 2)]
        settings = {"upload_reuse": "default", "upload_arena": "disabled",
                    "reuse_diagnostics_enabled": True,
                    "environment_contract": {
                        "RTS_SHADOW_UPLOAD_REUSE": None,
                        "RTS_SHADOW_UPLOAD_ARENA": "0",
                        "RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR": "run_local_path"}}
        for report in reports:
            report["identity"]["shadow_experiment"] = dict(settings)
        reports[-1]["identity"]["shadow_experiment"] = dict(settings)
        reports[-1]["identity"]["shadow_experiment"]["upload_reuse"] = "disabled"
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["data_comparable"])
        self.assertTrue(any("shadow_experiment mismatch" in reason
                            for rejection in result["rejections"] for reason in rejection["reasons"]))


class ShadowExperimentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".shadow-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def config(reuse="default", arena="default", diagnostics=False, path=None):
        return {"schema": "ggc.shadow-upload-experiment.v1",
                "upload_reuse": reuse, "upload_arena": arena,
                "reuse_diagnostics_enabled": diagnostics,
                "diagnostics_path": path,
                "environment_values": {
                    "RTS_SHADOW_UPLOAD_REUSE": "0" if reuse == "disabled" else None,
                    "RTS_SHADOW_UPLOAD_ARENA": "0" if arena == "disabled" else None,
                    "RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR": path}}

    def test_legacy_receipt_has_no_shadow_experiment(self):
        self.assertEqual(r1.shadow_experiment({}, self.root), (None, None))

    def test_new_controller_receipt_cannot_omit_shadow_experiment(self):
        with self.assertRaisesRegex(r1.Reject, "missing shadow experiment settings"):
            r1.shadow_experiment({"controller_script_sha256": "a" * 64}, self.root)

    def test_new_default_options_are_recorded_without_overriding_environment(self):
        settings, path = r1.shadow_experiment(
            {"shadow_experiment": self.config()}, self.root)
        self.assertIsNone(path)
        self.assertEqual(settings["upload_reuse"], "default")
        self.assertEqual(settings["upload_arena"], "default")
        self.assertIsNone(settings["environment_contract"]["RTS_SHADOW_UPLOAD_REUSE"])
        self.assertIsNone(settings["environment_contract"]["RTS_SHADOW_UPLOAD_ARENA"])

    def test_disabled_options_are_recorded_as_zero_environment_values(self):
        settings, _ = r1.shadow_experiment(
            {"shadow_experiment": self.config("disabled", "disabled")}, self.root)
        self.assertEqual(settings["environment_contract"]["RTS_SHADOW_UPLOAD_REUSE"], "0")
        self.assertEqual(settings["environment_contract"]["RTS_SHADOW_UPLOAD_ARENA"], "0")

    def test_diagnostics_must_use_existing_owned_shadow_directory(self):
        owned = self.root / "shadow"
        owned.mkdir()
        cfg = self.config(diagnostics=True, path=str(owned))
        settings, path = r1.shadow_experiment({"shadow_experiment": cfg}, self.root)
        self.assertEqual(path, str(owned))
        self.assertEqual(settings["environment_contract"][
            "RTS_SHADOW_STREAM_REUSE_DIAGNOSTICS_DIR"], "run_local_path")

    def test_diagnostics_path_outside_run_is_rejected(self):
        (self.root / "shadow").mkdir()
        outside = self.root.parent / (self.root.name + "-outside")
        outside.mkdir()
        try:
            with self.assertRaisesRegex(r1.Reject, "outside the owned run directory"):
                r1.shadow_experiment(
                    {"shadow_experiment": self.config(diagnostics=True,
                                                       path=str(outside))}, self.root)
        finally:
            outside.rmdir()

    def test_environment_mapping_must_match_declared_options(self):
        cfg = self.config()
        cfg["environment_values"]["RTS_SHADOW_UPLOAD_REUSE"] = "0"
        with self.assertRaisesRegex(r1.Reject, "environment values differ"):
            r1.shadow_experiment({"shadow_experiment": cfg}, self.root)


class MultipageContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".multipage-")
        self.root = Path(self.tmp.name)
        self.proof_path = self.root / "original-proof.json"
        self.ready = {"run": str(self.root), "created_utc": "2026-10-04T12:00:01Z",
                      "controller_script_sha256": "a" * 64,
                      "expected_executable_sha256": "b" * 64,
                      "snapshot_identity": "c" * 64,
                      "launcher": {"parent_pid": 10, "started_utc": "2026-10-04T12:00:02Z"},
                      "shadow_experiment": ShadowExperimentTests.config()}
        self.proof = {"schema": "ggc.shadow-multipage-environment-proof.v1",
                      "run": str(self.root), "created_utc": "2026-10-04T12:00:00Z",
                      "fresh_shell_pid": 10, "controller_sha256": "a" * 64,
                      "executable_sha256": "b" * 64, "source_snapshot_sha256": "c" * 64,
                      "multipage_variable": "RTS_SHADOW_MULTIPAGE_ARENA",
                      "inherited_entries_removed": 0,
                      "verified_process_environment_absent": True, "effective_request": "default"}

    def tearDown(self):
        self.tmp.cleanup()

    def capture(self):
        self.proof_path.write_text(json.dumps(self.proof), encoding="utf-8")
        return r1.shadow_multipage(self.ready, self.root, self.proof_path)

    def v2(self, request="default"):
        cfg = ShadowExperimentTests.config()
        cfg.update(schema="ggc.shadow-upload-experiment.v2", multipage_arena=request)
        cfg["environment_values"]["RTS_SHADOW_MULTIPAGE_ARENA"] = "0" if request == "disabled" else None
        self.ready["shadow_experiment"] = cfg
        return cfg

    def reports(self):
        return [comparison_run("baseline", 1), comparison_run("baseline", 2),
                comparison_run("candidate", 1), comparison_run("candidate", 2)]

    def compare(self, reports, proofs=None):
        with patch.object(r1, "analyze", side_effect=reports) as mocked:
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500, proofs)
        return result, mocked

    def test_v1_missing_proof_is_explicitly_unknown(self):
        self.assertEqual(r1.shadow_multipage(self.ready, self.root),
                         ("unknown", {"source": "uncaptured"}))

    def test_original_default_proof_preserves_raw_provenance(self):
        request, provenance = self.capture()
        self.assertEqual(request, "default")
        self.assertEqual(provenance["sha256"], r1.digest(self.proof_path))
        self.assertEqual(provenance["path"], str(self.proof_path.resolve()))

    def test_v2_default_and_disabled_match_exact_environment(self):
        for request in ("default", "disabled"):
            cfg = self.v2(request)
            settings, _ = r1.shadow_experiment(self.ready, self.root)
            self.assertEqual(settings["upload_arena"], "default")
            self.assertEqual(r1.shadow_multipage(self.ready, self.root)[0], request)
            self.assertEqual(cfg["environment_values"]["RTS_SHADOW_MULTIPAGE_ARENA"],
                             "0" if request == "disabled" else None)

    def test_v2_missing_or_unknown_request_rejected(self):
        for request in (None, "unknown", "enabled", False, 0):
            self.v2(request)
            with self.assertRaises(r1.Reject): r1.shadow_experiment(self.ready, self.root)
        del self.ready["shadow_experiment"]["multipage_arena"]
        with self.assertRaises(r1.Reject): r1.shadow_experiment(self.ready, self.root)

    def test_v2_environment_missing_extra_or_mismatched_rejected(self):
        for mode in ("missing", "extra", "wrong"):
            cfg = self.v2()
            env = cfg["environment_values"]
            if mode == "missing": del env["RTS_SHADOW_MULTIPAGE_ARENA"]
            elif mode == "extra": env["unexpected"] = None
            else: env["RTS_SHADOW_MULTIPAGE_ARENA"] = "0"
            with self.assertRaises(r1.Reject): r1.shadow_experiment(self.ready, self.root)

    def test_original_proof_run_and_hash_bindings_rejected(self):
        for key in ("run", "controller_sha256", "executable_sha256", "source_snapshot_sha256"):
            original = self.proof[key]
            self.proof[key] = str(self.root / "other") if key == "run" else "d" * 64
            with self.assertRaises(r1.Reject): self.capture()
            self.proof[key] = original

    def test_original_proof_parent_and_count_require_json_integers(self):
        for key, value in (("fresh_shell_pid", 11), ("fresh_shell_pid", 10.0),
                           ("fresh_shell_pid", True), ("fresh_shell_pid", "10"),
                           ("inherited_entries_removed", -1), ("inherited_entries_removed", 0.0),
                           ("inherited_entries_removed", False)):
            original = self.proof[key]
            self.proof[key] = value
            with self.assertRaises(r1.Reject): self.capture()
            self.proof[key] = original

    def test_original_proof_requires_default_absent_request(self):
        for key, value in (("effective_request", "disabled"), ("effective_request", "unknown"),
                           ("verified_process_environment_absent", False),
                           ("verified_process_environment_absent", 1),
                           ("multipage_variable", "OTHER"), ("schema", "unknown")):
            original = self.proof[key]
            self.proof[key] = value
            with self.assertRaises(r1.Reject): self.capture()
            self.proof[key] = original

    def test_original_proof_missing_fields_rejected(self):
        for key in list(self.proof):
            original = self.proof.pop(key)
            with self.assertRaises(r1.Reject): self.capture()
            self.proof[key] = original

    def test_original_proof_missing_malformed_and_duplicate_json_rejected(self):
        with self.assertRaises(r1.Reject): r1.shadow_multipage(self.ready, self.root, self.proof_path)
        for raw in ("{", "[]", '{"schema":"x","schema":"y"}'):
            self.proof_path.write_text(raw, encoding="utf-8")
            with self.assertRaises(r1.Reject): r1.shadow_multipage(self.ready, self.root, self.proof_path)

    def test_original_proof_timestamp_must_precede_launch_and_ready_capture(self):
        for value in ("bad", "2026-10-04T12:00:00", "2026-10-04T12:00:02Z",
                      "2026-10-04T12:00:01.0000001Z", "2026-13-04T12:00:00Z"):
            self.proof["created_utc"] = value
            with self.assertRaises(r1.Reject): self.capture()
        self.ready["launcher"]["started_utc"] = "2026-10-04T12:00:00.0000002Z"
        self.ready["created_utc"] = "2026-10-04T12:00:00.0000001Z"
        self.proof["created_utc"] = "2026-10-04T12:00:00.0000001Z"
        self.assertEqual(self.capture()[0], "default")
        self.ready["launcher"]["started_utc"] = self.proof["created_utc"]
        with self.assertRaises(r1.Reject): self.capture()

    def test_original_proof_cannot_replace_v2_capture(self):
        self.v2()
        with self.assertRaises(r1.Reject): self.capture()

    def test_comparison_rejects_missing_unknown_and_disabled_mismatch(self):
        for value in (None, "unknown", "disabled"):
            reports = self.reports()
            reports[-1]["identity"]["shadow_multipage_arena"] = value
            self.assertFalse(self.compare(reports)[0]["data_comparable"])

    def test_v1_proof_and_v2_default_compare_by_semantics(self):
        v1 = r1.shadow_experiment(self.ready, self.root)[0]
        self.capture()
        self.v2()
        v2 = r1.shadow_experiment(self.ready, self.root)[0]
        self.assertEqual(v1, v2)
        reports = self.reports()
        for index, report in enumerate(reports):
            report["identity"].update(shadow_experiment=v1 if index < 2 else v2,
                                      shadow_multipage_evidence={"source": "original" if index < 2 else "controller_v2"})
        self.assertTrue(self.compare(reports)[0]["data_comparable"])

    def test_comparison_routes_explicit_proof_to_only_named_run(self):
        result, mocked = self.compare(self.reports(), [("b1", "original.json")])
        self.assertTrue(result["data_comparable"])
        self.assertEqual(mocked.call_args_list[0].kwargs, {"multipage_proof": "original.json"})
        self.assertEqual(mocked.call_args_list[1].kwargs, {})

    def test_comparison_rejects_duplicate_or_unlisted_proof_bindings(self):
        for proofs in ([("other", "proof")], [("b1", "proof"), ("b1", "other-proof")]):
            with self.assertRaises(r1.Reject): self.compare(self.reports(), proofs)

    def test_analyze_cli_rejects_repeated_proof_before_analysis(self):
        for second in ("missing.json", "valid.json"):
            argv = ["analyze_r1.py", "analyze", str(self.root),
                    "--multipage-proof", "missing.json", "--multipage-proof", second]
            with patch.object(sys, "argv", argv), patch.object(sys, "stderr") as stderr, patch.object(r1, "analyze") as mocked:
                self.assertEqual(r1.main(), 2)
                mocked.assert_not_called()
                self.assertTrue(any("duplicate multipage proof option" in str(call)
                                    for call in stderr.write.call_args_list))

    def test_receipt_duplicate_json_keys_rejected(self):
        self.proof_path.write_text('{"shadow_experiment":{},"shadow_experiment":{}}', encoding="utf-8")
        with self.assertRaisesRegex(r1.Reject, "duplicate JSON fields"): r1.load(self.proof_path)

    def test_window_json_integer_strings_floats_and_bools_rejected(self):
        ready = {"game": {"pid": 77, "parent_pid": 20}}
        base = {"schema": "ggc.passive-window-observations.v1", "normal_exit": True,
                "status": "sampled", "game": ready["game"], "qpc_frequency": 1000,
                "sample_count": 1, "observations": [{"qpc_begin": 10000, "qpc_end": 11000,
                    "game_pid": 77, "window_pid": 77, "foreground_pid": 77,
                    "status": "observed", "read_error": None, "visible": True,
                    "iconic": False, "foreground": True}]}
        for field in ("qpc_frequency", "sample_count", "qpc_begin", "qpc_end", "game_pid", "window_pid", "foreground_pid", "pid", "parent_pid"):
            for kind in (str, float, bool):
                monitor = json.loads(json.dumps(base))
                target = monitor if field in ("qpc_frequency", "sample_count") else (monitor["game"] if field in ("pid", "parent_pid") else monitor["observations"][0])
                target[field] = kind(target[field])
                with self.assertRaisesRegex(r1.Reject, "integer JSON value"):
                    r1.window_evidence(monitor, ready, {"qpc_frequency": 1000, "begin": 10000, "stop": 11000})


class PresentParserTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".synthetic-")
        self.path = Path(self.tmp.name) / "present-frame-timing-test.csv"
        self.phase = {"qpc_frequency": 1000, "begin": 10000, "stop": 14000}
        self.ready = {"game": {"pid": 77}}

    def tearDown(self):
        self.tmp.cleanup()

    def run_present(self, ends, **kwargs):
        make_csv(self.path, ends, **kwargs)
        return r1.present(self.path, self.phase, self.ready)

    def test_footer_count_mismatch_rejected(self):
        make_csv(self.path, [10400, 10800], export_delta=1)
        with self.assertRaisesRegex(r1.Reject, "footer counts"):
            r1.present(self.path, self.phase, self.ready)

    def test_fractional_csv_footer_count_and_qpc_are_rejected(self):
        for row_type, status, field, value in (
                ("summary", "calls", "count", "1.5"),
                ("summary", "calls", "count", "01"),
                ("present", None, "present_start_qpc", "10399.5")):
            with self.subTest(field=field):
                make_csv(self.path, [10400])
                with self.path.open(newline="", encoding="utf-8") as f:
                    reader = csv.DictReader(f)
                    fieldnames = reader.fieldnames
                    rows = list(reader)
                target = next(row for row in rows if row["row_type"] == row_type and
                              (status is None or row["status"] == status))
                target[field] = value
                with self.path.open("w", newline="", encoding="utf-8") as f:
                    writer = csv.DictWriter(f, fieldnames=fieldnames)
                    writer.writeheader()
                    writer.writerows(rows)
                with self.assertRaisesRegex(r1.Reject, "must be an integer"):
                    r1.present(self.path, self.phase, self.ready)

    def test_nonmonotonic_completion_rejected(self):
        make_csv(self.path, [10400, 10800, 10700, 11200])
        with self.assertRaisesRegex(r1.Reject, "nonmonotonic"):
            r1.present(self.path, self.phase, self.ready)

    def test_repeated_long_gaps_qualify_as_failure(self):
        _, reasons = self.run_present([10400, 11400, 12400, 13600])
        self.assertTrue(any(x.startswith("repeated_prolonged_present_gaps") for x in reasons))

    def test_non_success_in_measured_phase_is_excluded_and_reported(self):
        report, reasons = self.run_present([10400, 10800, 11200, 11600, 12000, 12400, 12800, 13200, 13600], positive=(5,))
        self.assertEqual(report["excluded_non_s_ok_inside_phase"], 1)
        self.assertIn("non_s_ok_inside_phase:1", reasons)

    def test_failed_present_inside_phase_remains_a_present_gate_failure(self):
        make_csv(self.path, [10400, 10800], failed=(1,))
        _, reasons = r1.present(self.path, self.phase, self.ready)
        self.assertIn("non_s_ok_inside_phase:1", reasons)

    def test_visible_background_window_is_allowed_and_reported(self):
        ready={"game":{"pid":77,"started_utc":"t","executable":"game.exe","sha256":"a"*64,"parent_pid":20}}
        observations=[]
        for qpc in (9900,10900,11900,12900,13900):
            observations.append({"qpc_begin":qpc,"qpc_end":qpc+10,"game_pid":77,"window_pid":77,
                                "foreground_pid":88,"visible":True,"iconic":False,"foreground":False,
                                "status":"observed","read_error":None})
        monitor={"schema":"ggc.passive-window-observations.v1","normal_exit":True,"status":"sampled",
                 "sample_count":len(observations),"qpc_frequency":1000,"game":ready["game"],"observations":observations}
        result,reasons=r1.window_evidence(monitor,ready,self.phase)
        self.assertEqual(reasons,[])
        self.assertEqual(result["mode"],"background")
        self.assertEqual(result["background_count"],4)

    def test_iconic_window_is_rejected(self):
        ready={"game":{"pid":77,"started_utc":"t","executable":"game.exe","sha256":"a"*64,"parent_pid":20}}
        observations=[]
        for qpc in (9900,10900,11900,12900,13900):
            observations.append({"qpc_begin":qpc,"qpc_end":qpc+10,"game_pid":77,"window_pid":77,
                                "foreground_pid":77,"visible":True,"iconic":qpc==11900,"foreground":True,
                                "status":"observed","read_error":None})
        monitor={"schema":"ggc.passive-window-observations.v1","normal_exit":True,"status":"sampled",
                 "sample_count":len(observations),"qpc_frequency":1000,"game":ready["game"],"observations":observations}
        _,reasons=r1.window_evidence(monitor,ready,self.phase)
        self.assertIn("window_hidden_iconic_or_identity_mismatch",reasons)

    def test_comparison_rejects_different_fixture_state(self):
        def report(role, number, state):
            pid = (1000 if role == "baseline" else 2000) + number
            start_qpc = pid * 1000
            return {
                "qualified": True, "run_directory": f"{role}-{number}",
                "identity": {"source_commit": role, "snapshot_identity": role,
                             "game_sha256": role, "launcher_sha256": "launch",
                             "telemetry": "minimalPresent", "source_options_sha256": "src",
                             "test_options_sha256": "opts", "settings": {"aa": 8},
                             "affinity": {"mask": "0xFFF"},
                             "captured_affinity": {"game": "0xFFF", "launcher": "0xFFF"},
                             "launcher_configuration": {"path": f"C:\\benchmark-runs\\{role}-{number}\\runtime\\launcher.lcf",
                                                        "sha256": "a" * 64,
                                                        "content": "RUN = . generalszh.exe"},
                             "window_mode": "foreground", "present_swap_interval": 0},
                "phase": {"qpc_frequency": 1000, "begin": start_qpc + 10000,
                          "stop": start_qpc + 14000},
                "workload_identity": {"seed": 637808953, "map": "arena", "map_crc": "abc",
                                      "map_size": 10, "camera": "(1,2,3)", "roster": [], "state": state},
                "present": {"owner": {"owner_session": 3, "process_id": pid,
                                       "owner_thread_id": 2, "owner_start_qpc": start_qpc},
                            "fps": 15.0, "per_second_fps": [15.0],
                            "gaps_ms": {"p95": 70.0, "p99": 80.0, "stall_count": 0},
                            "phase_edge_gap_ms": {"first": 10, "last": 10}},
            }
        reports = [report("baseline", 1, {"live": 512}), report("baseline", 2, {"live": 512}),
                   report("candidate", 1, {"live": 512}), report("candidate", 2, {"live": 511})]
        with patch.object(r1, "analyze", side_effect=reports):
            result = r1.comparison(["b1", "b2"], ["c1", "c2"], 500, 500)
        self.assertFalse(result["accepted"])
        self.assertTrue(any("state mismatch" in x["reasons"][0] for x in result["rejections"]))


class WindowCadenceTests(unittest.TestCase):
    def setUp(self):
        self.ready = {"game": {"pid": 77, "parent_pid": 20}}
        self.phase = {"qpc_frequency": 1000, "begin": 10000, "stop": 14000}

    def evidence(self, intervals):
        observations = [{"qpc_begin": begin, "qpc_end": end, "game_pid": 77,
                         "window_pid": 77, "foreground_pid": 77, "status": "observed",
                         "read_error": None, "visible": True, "iconic": False,
                         "foreground": True} for begin, end in intervals]
        monitor = {"schema": "ggc.passive-window-observations.v1", "normal_exit": True,
                   "status": "sampled", "game": self.ready["game"], "qpc_frequency": 1000,
                   "sample_count": len(observations), "observations": observations}
        return r1.window_evidence(monitor, self.ready, self.phase)

    def test_short_distinct_samples_allow_duration_and_cadence_boundaries(self):
        report, reasons = self.evidence([(10000, 10250), (11500, 11750), (13000, 13250)])
        self.assertEqual(reasons, [])
        self.assertEqual(report["observations_in_phase"], 3)

    def test_single_phase_spanning_observation_cannot_fake_coverage(self):
        with self.assertRaisesRegex(r1.Reject, "duration exceeds 250 ms"):
            self.evidence([(10000, 14000)])
        with self.assertRaisesRegex(r1.Reject, "duration exceeds 250 ms"):
            self.evidence([(10000, 10251), (11000, 11010), (12000, 12010), (13000, 13010)])

    def test_overlapping_duplicated_and_unordered_samples_are_rejected(self):
        for intervals in ([(10000, 10200), (10100, 10300), (13000, 13010)],
                          [(10000, 10000), (10000, 10000), (13000, 13010)],
                          [(11000, 11010), (10000, 10010), (13000, 13010)]):
            with self.assertRaisesRegex(r1.Reject, "unordered, overlapping, or duplicated"):
                self.evidence(intervals)

    def test_short_phase_still_requires_multiple_distinct_samples(self):
        self.phase["stop"] = 10200
        with self.assertRaisesRegex(r1.Reject, "multiple distinct phase samples"):
            self.evidence([(10000, 10200)])
        _, reasons = self.evidence([(10000, 10010), (10100, 10110)])
        self.assertEqual(reasons, [])

    def test_short_samples_cannot_hide_cadence_or_edge_gaps(self):
        _, reasons = self.evidence([(10000, 10010), (11501, 11511), (13000, 13010)])
        self.assertIn("window_observation_sampling_gap_gt_1_5s", reasons)
        _, reasons = self.evidence([(12000, 12010), (13000, 13010)])
        self.assertIn("window_observation_edge_gap_gt_1_5s", reasons)


class GpuEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".gpu-")
        self.root = Path(self.tmp.name)
        self.path = self.root / "gpu-frame-timing-77-2-100.csv"
        self.ready = {"game": {"pid": 77}}
        self.phase = {"qpc_frequency": 1000, "begin": 10000, "stop": 11000}
        present = gpu_present_row()
        self.present_report = {"owner": {"process_id": 77, "owner_session": 3},
                               "completions": 1}
        self.present_index = {"by_qpc": {(10500, 10510, 1000): [present]},
                              "by_frame": {(1, 1): [present]}}

    def tearDown(self):
        self.tmp.cleanup()

    def evidence(self, path=None, frames=None, **kwargs):
        target = self.path if path is None else path
        write_gpu_csv(target, [gpu_frame()] if frames is None else frames, **kwargs)
        return r1.gpu_evidence(target, self.ready, self.phase,
                               self.present_report, self.present_index)

    def outside_phase_failed_present_frames(self, count=1):
        failed_presents = []
        failed_frames = []
        by_qpc = {}
        by_frame = {}
        for ordinal in range(1, count + 1):
            start = 9000 + (ordinal - 1) * 10
            end = start + 1
            failed_present = gpu_present_row(epoch=1, ordinal=ordinal, start=start, end=end,
                                             present_ordinal=ordinal, present_hresult=-1)
            failed_presents.append(failed_present)
            by_qpc[(start, end, 1000)] = [failed_present]
            by_frame[(1, ordinal)] = [failed_present]
            failed_frames.append(gpu_frame(epoch=1, ordinal=ordinal, start=start, end=end,
                                           status="present_failed", present_hresult=-1))
        measured_ordinal = count + 1
        measured_present = gpu_present_row(epoch=1, ordinal=measured_ordinal,
                                           present_ordinal=measured_ordinal)
        self.present_report["completions"] = 1
        by_qpc[(10500, 10510, 1000)] = [measured_present]
        by_frame[(1, measured_ordinal)] = [measured_present]
        self.present_index = {"by_qpc": by_qpc, "by_frame": by_frame}
        return failed_frames + [gpu_frame(epoch=1, ordinal=measured_ordinal)]

    def pre_present_failed_frames(self, count=1):
        failed_frames = []
        for ordinal in range(1, count + 1):
            failed_frames.append(gpu_frame(
                epoch=1, ordinal=ordinal, start=0, end=0, status="present_failed",
                present_called=0, present_hresult=0, cpu_present_ms=-1.0,
                cpu_qpc_frequency=0,
                gpu_total_ms=-1.0, gpu_scene_ms=-1.0, gpu_resolve_ms=-1.0,
                gpu_gamma_ms=-1.0))
        measured_ordinal = count + 1
        measured_present = gpu_present_row(epoch=1, ordinal=measured_ordinal)
        self.present_report["completions"] = 1
        self.present_index = {
            "by_qpc": {(10500, 10510, 1000): [measured_present]},
            "by_frame": {(1, measured_ordinal): [measured_present]},
        }
        return failed_frames + [gpu_frame(epoch=1, ordinal=measured_ordinal)]

    def test_valid_export_proves_effective_quality_and_hardware_identity(self):
        result = self.evidence()
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["effective_quality"]["msaa_samples"], 8)
        self.assertIn("sampled_backend_frame_only", result["effective_quality"]["scope"])
        self.assertTrue(result["adapter"]["native_hardware_adapter"])
        self.assertEqual(result["frame_identity"]["ambiguous_matches"], 0)

    def test_gpu_timestamp_coverage_may_be_lower_than_present_rows(self):
        second = gpu_present_row(epoch=1, ordinal=2, start=10600, end=10610)
        self.present_report["completions"] = 2
        self.present_index["by_qpc"][(10600, 10610, 1000)] = [second]
        self.present_index["by_frame"][(1, 2)] = [second]
        result = self.evidence()
        self.assertEqual(result["measurement_phase"]["successful_present_rows"], 2)
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)
        self.assertEqual(result["measurement_phase"]["timestamp_coverage_fraction"], 0.5)

    def test_software_adapter_is_rejected(self):
        with self.assertRaisesRegex(r1.Reject, "verified native hardware adapter"):
            self.evidence(adapters={1: {"adapter_software": 1}})

    def test_adapter_change_across_epochs_is_rejected(self):
        second_present = gpu_present_row(epoch=2, ordinal=1, start=10600, end=10610,
                                         present_ordinal=2)
        self.present_report["completions"] = 2
        self.present_index["by_qpc"][(10600, 10610, 1000)] = [second_present]
        self.present_index["by_frame"][(2, 1)] = [second_present]
        frames = [gpu_frame(), gpu_frame(epoch=2, ordinal=1, start=10600, end=10610)]
        with self.assertRaisesRegex(r1.Reject, "identity changed across device epochs"):
            self.evidence(frames=frames, adapters={2: {"adapter_luid_low": 99999}})

    def test_debug_layer_change_across_epochs_is_rejected(self):
        second_present = gpu_present_row(epoch=2, ordinal=1, start=10600, end=10610,
                                         present_ordinal=2)
        self.present_report["completions"] = 2
        self.present_index["by_qpc"][(10600, 10610, 1000)] = [second_present]
        self.present_index["by_frame"][(2, 1)] = [second_present]
        frames = [gpu_frame(), gpu_frame(epoch=2, ordinal=1, start=10600, end=10610)]
        with self.assertRaisesRegex(r1.Reject, "identity changed across device epochs"):
            self.evidence(frames=frames, adapters={2: {"device_debug_layer": 1}})

    def test_invalid_or_disjoint_query_status_keeps_backend_quality_sample(self):
        for status in ("invalid", "disjoint"):
            with self.subTest(status=status):
                result = self.evidence(frames=[gpu_frame(status=status)])
                self.assertEqual(result["status"], "verified")
                self.assertEqual(result["measurement_phase"]["timestamp_coverage_fraction"], 0.0)
                self.assertEqual(result["effective_quality"]["msaa_samples"], 8)

    def test_truthful_negative_hresult_present_call_can_hide_shutdown_outcome_outside_phase(self):
        frames = self.outside_phase_failed_present_frames()
        self.assertEqual(frames[0]["present_called"], 1)
        self.assertLess(frames[0]["present_hresult"], 0)
        self.assertGreater(frames[0]["present_start_qpc"], 0)
        self.assertGreaterEqual(frames[0]["present_end_qpc"], frames[0]["present_start_qpc"])
        self.assertGreater(frames[0]["cpu_qpc_frequency"], 0)
        result = self.evidence(frames=frames, counters={"shutdown_pending": 1, "cancelled": 1})
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["csv"]["present_joined_records"], 2)
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)

    def test_finalized_unpresented_row_is_visible_invalid_outcome(self):
        unpresented = gpu_frame(epoch=1, ordinal=1, start=0, end=0,
                                status="unpresented", present_called=0, present_hresult=0,
                                cpu_present_ms=-1.0, gpu_total_ms=-1.0,
                                gpu_scene_ms=-1.0, gpu_resolve_ms=-1.0,
                                gpu_gamma_ms=-1.0)
        measured_present = gpu_present_row(epoch=1, ordinal=2)
        self.present_report["completions"] = 1
        self.present_index = {
            "by_qpc": {(10500, 10510, 1000): [measured_present]},
            "by_frame": {(1, 2): [measured_present]},
        }
        result = self.evidence(frames=[unpresented, gpu_frame(epoch=1, ordinal=2)],
                               counters={"cancelled": 1})
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)

    def test_pre_present_resolve_gamma_failures_can_precede_successful_sample(self):
        # Both renderer paths cancel before beforePresent; their exported rows
        # therefore have no Present call, zero QPC identity, and unresolved GPU times.
        frames = self.pre_present_failed_frames(count=2)
        for failed in frames[:2]:
            self.assertEqual(failed["present_called"], 0)
            self.assertEqual((failed["present_start_qpc"], failed["present_end_qpc"],
                              failed["cpu_qpc_frequency"], failed["present_hresult"],
                              failed["cpu_present_ms"]), (0, 0, 0, 0, -1.0))
        result = self.evidence(frames=frames,
                               counters={"invalid": 2, "cancelled": 2})
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["csv"]["present_joined_records"], 1)
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)

    def test_present_failed_with_s_ok_call_is_rejected_even_without_counter_excess(self):
        frames = self.outside_phase_failed_present_frames()
        failed_present = self.present_index["by_qpc"][(9000, 9001, 1000)][0]
        frames[0]["present_hresult"] = 0
        failed_present["present_hresult"] = 0
        with self.assertRaisesRegex(r1.Reject, "present_failed row with a Present call lacks a negative HRESULT"):
            self.evidence(frames=frames)

    def test_present_failed_without_call_rejects_nondefault_present_fields_even_without_counter_excess(self):
        frames = self.pre_present_failed_frames()
        frames[0]["present_hresult"] = -1
        frames[0]["cpu_qpc_frequency"] = 1000
        with self.assertRaisesRegex(r1.Reject, "present_failed row without a Present call has non-default Present fields"):
            self.evidence(frames=frames, counters={"cancelled": 1})

    def test_present_failed_call_keeps_strict_qpc_clock_validation(self):
        frames = self.outside_phase_failed_present_frames()
        frames[0]["present_end_qpc"] = frames[0]["present_start_qpc"] - 1
        with self.assertRaisesRegex(r1.Reject, "GPU Present QPC interval is invalid"):
            self.evidence(frames=frames, counters={"shutdown_pending": 1})

    def test_pre_present_failure_cannot_be_claimed_as_invalid_and_shutdown(self):
        frames = self.pre_present_failed_frames()
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"invalid": 1, "shutdown_pending": 1,
                                                    "cancelled": 1})

    def test_hidden_readiness_loss_on_failed_present_is_accepted_once(self):
        frames = self.outside_phase_failed_present_frames()
        result = self.evidence(frames=frames, counters={"invalid": 1, "readiness_failures": 1})
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)

    def test_hidden_completed_query_on_failed_present_is_accepted_once(self):
        frames = self.outside_phase_failed_present_frames()
        result = self.evidence(frames=frames, counters={"complete": 2})
        self.assertEqual(result["status"], "verified")

    def test_hidden_disjoint_is_already_in_invalid_and_is_not_double_counted(self):
        frames = self.outside_phase_failed_present_frames()
        result = self.evidence(frames=frames, counters={"invalid": 1, "disjoint": 1})
        self.assertEqual(result["status"], "verified")

    def test_hidden_invalid_loss_cannot_be_claimed_again_as_shutdown(self):
        frames = self.outside_phase_failed_present_frames()
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"invalid": 1, "shutdown_pending": 1,
                                                    "readiness_failures": 1})

    def test_hidden_completed_query_cannot_be_claimed_again_as_shutdown(self):
        frames = self.outside_phase_failed_present_frames()
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"complete": 2, "shutdown_pending": 1})

    def test_hidden_complete_and_invalid_cannot_both_claim_one_failed_present_row(self):
        frames = self.outside_phase_failed_present_frames()
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"complete": 2, "invalid": 1})

    def test_shutdown_pending_footer_cannot_exceed_visible_and_hidden_failed_present_rows(self):
        frames = self.outside_phase_failed_present_frames()
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"shutdown_pending": 2})

    def test_exclusive_lifecycle_counters_cannot_double_claim_one_failed_present_row(self):
        frames = self.outside_phase_failed_present_frames()
        for counter in ("resize_dropped", "device_dropped", "shutdown_pending"):
            with self.subTest(counter=counter):
                result = self.evidence(frames=frames, counters={counter: 1})
                self.assertEqual(result["status"], "verified")
        with self.assertRaisesRegex(r1.Reject, "exclusive hidden outcomes exceed present_failed rows"):
            self.evidence(frames=frames, counters={"resize_dropped": 1,
                                                    "shutdown_pending": 1})

    def test_distinct_failed_present_rows_can_account_for_distinct_lifecycle_losses(self):
        frames = self.outside_phase_failed_present_frames(count=2)
        result = self.evidence(frames=frames, counters={"resize_dropped": 1,
                                                       "shutdown_pending": 1})
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["measurement_phase"]["gpu_joined_successful_rows"], 1)

    def test_shutdown_pending_footer_cannot_understate_visible_shutdown_rows(self):
        visible_shutdown = gpu_frame(epoch=1, ordinal=2, start=0, end=0,
                                     status="shutdown_pending", present_called=0)
        with self.assertRaisesRegex(r1.Reject, "shutdown_pending count is below visible rows"):
            self.evidence(frames=[gpu_frame(), visible_shutdown],
                          counters={"shutdown_pending": 0})

    def test_invalid_footer_cannot_understate_visible_readiness_failure_rows(self):
        visible_readiness_failure = gpu_frame(epoch=1, ordinal=2, start=0, end=0,
                                              status="readiness_failed", present_called=0)
        with self.assertRaisesRegex(r1.Reject, "invalid count is below visible"):
            self.evidence(frames=[gpu_frame(), visible_readiness_failure],
                          counters={"invalid": 0})

    def test_wrong_effective_sample_count_is_rejected(self):
        with self.assertRaisesRegex(r1.Reject, "effective MSAA is not 8x"):
            self.evidence(frames=[gpu_frame(samples=4)])

    def test_wrong_effective_resolution_or_missing_resolve_is_rejected(self):
        for fields in ({"width": 1280}, {"resolve_applied": 0}):
            with self.subTest(fields=fields), self.assertRaisesRegex(r1.Reject, "effective resolution|effective MSAA"):
                self.evidence(frames=[gpu_frame(**fields)])

    def test_missing_gpu_footer_is_rejected(self):
        with self.assertRaisesRegex(r1.Reject, "GPU footer is missing or truncated"):
            self.evidence(omit_export_end=True)

    def test_truncated_gpu_footer_row_is_rejected(self):
        write_gpu_csv(self.path, [gpu_frame()], truncate_export_end=True)
        with self.assertRaisesRegex(r1.Reject, "truncated or malformed row"):
            r1.gpu_evidence(self.path, self.ready, self.phase,
                            self.present_report, self.present_index)

    def test_export_session_pid_must_match_ready_and_present_owner(self):
        wrong_pid = self.root / "gpu-frame-timing-78-2-100.csv"
        write_gpu_csv(wrong_pid, [gpu_frame()])
        with self.assertRaisesRegex(r1.Reject, "session PID differs"):
            r1.gpu_evidence(wrong_pid, self.ready, self.phase,
                            self.present_report, self.present_index)

    def test_gpu_frame_must_join_to_the_present_owner_session(self):
        self.present_report["owner"]["owner_session"] = 4
        with self.assertRaisesRegex(r1.Reject, "session/PID/HRESULT"):
            self.evidence()

    def test_malformed_export_session_name_is_rejected(self):
        malformed = self.root / "gpu-frame-timing-session.csv"
        write_gpu_csv(malformed, [gpu_frame()])
        with self.assertRaisesRegex(r1.Reject, "session filename is malformed"):
            r1.gpu_evidence(malformed, self.ready, self.phase,
                            self.present_report, self.present_index)

    def test_frame_qpc_must_match_the_same_present_frame_identity(self):
        with self.assertRaisesRegex(r1.Reject, "does not identify one Present row"):
            self.evidence(frames=[gpu_frame(start=10501)])


if __name__ == "__main__":
    unittest.main()
