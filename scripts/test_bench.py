#!/usr/bin/env python3
"""Tests for bench_run.py, bench_summary.py and bench_plot.py.

Unit tests use synthetic records, so they need no build. With --build DIR, an
end-to-end smoke campaign (bench/matrix-smoke.json) also runs the real
binaries through the runner, the summary and, if matplotlib is installed, the
plots. Usage: scripts/test_bench.py [--build DIR]
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_run  # noqa: E402
import bench_summary as summary  # noqa: E402

BUILD = None

MANIFEST = {
    "glibc": {"library": None, "runEnv": {},
              "hugepageEnv": {"GLIBC_TUNABLES": "glibc.malloc.hugetlb=1"}},
    "tcmalloc": {"library": "/lib/libtcmalloc.so",
                 "runEnv": {"GLIBC_TUNABLES": "glibc.pthread.rseq=0"},
                 "hugepageEnv": {}},
}


def campaign(cells, hugepages=True, counters="required", cpu=2):
    return {"id": "test", "runs": 10, "cpu": cpu, "counters": counters,
            "hugepages": hugepages, "manifest": MANIFEST,
            "matrix": {"require": {"thp": "always", "governor": "performance"},
                       "configurations": {}, "comparisons": {}},
            "cells": cells, "machine": {}, "git": {}}


def chase_record(cell, ns=20.0, allocator="glibc", env=None):
    return {
        "schema": summary.SCHEMA, "workload": "chase",
        "build": {"contractSemantic": "ignore"},
        "allocator": {"name": allocator},
        "allocatorEnv": env if env is not None
        else {"GLIBC_TUNABLES": "glibc.malloc.hugetlb=1"},
        "region": {"migrated": False, "interrupts": 3,
                   "contextSwitches": {"voluntary": 0, "involuntary": 1}},
        "system": {"thp": "always", "governor": "performance",
                   "warmup": {"converged": True}},
        "counters": {"available": True,
                     "region": {"status": "ok",
                                "values": {"llcLoadMisses": 500,
                                           "dtlbLoadWalks": 0}}},
        "memory": {"droppedSamples": 0, "startup": {"heldAnonKiB": 1000},
                   "inputBytes": 0, "after": None, "samples": []},
        "metrics": {"visits": 1000, "nsPerVisit": ns, "cyclesPerVisit": ns * 4,
                    "hotSetBytes": 4096, "seam": {"leakedBytes": 0}},
        "params": {},
        "run": {"cell": cell["id"], "repetition": 0},
    }


CELL = {"id": "chase/x/glibc", "workload": "chase", "variant": "x",
        "configuration": "glibc", "allocator": "glibc", "sweep": None}


class Validation(unittest.TestCase):
    def reasons(self, record, camp=None):
        camp = camp or campaign([CELL])
        return summary.rejections(record, camp, {CELL["id"]: CELL})

    def test_clean_record_is_accepted(self):
        self.assertEqual(self.reasons(chase_record(CELL)), [])

    def test_each_rule_rejects(self):
        breakages = {
            "schema": lambda r: r.update(schema=1),
            "contract semantic": lambda r: r["build"].update(contractSemantic="enforce"),
            "served by": lambda r: r["allocator"].update(name="jemalloc"),
            "missing GLIBC_TUNABLES": lambda r: r.update(allocatorEnv={}),
            "migrated": lambda r: r["region"].update(migrated=True),
            "warmup": lambda r: r["system"]["warmup"].update(converged=False),
            "counters": lambda r: r["counters"]["region"].update(status="multiplexed"),
            "leaked": lambda r: r["metrics"]["seam"].update(leakedBytes=8),
            "dropped": lambda r: r["memory"].update(droppedSamples=2),
            "THP": lambda r: r["system"].update(thp="madvise"),
            "governor": lambda r: r["system"].update(governor="powersave"),
            "context switches": lambda r: r["region"]["contextSwitches"].update(
                involuntary=600),
            "not a run": lambda r: r["run"].update(cell="other"),
        }
        for expected, breaking in breakages.items():
            record = copy.deepcopy(chase_record(CELL))
            breaking(record)
            reasons = self.reasons(record)
            self.assertTrue(any(expected in reason for reason in reasons),
                            f"{expected}: got {reasons}")

    def test_tunables_are_matched_as_a_set(self):
        cell = dict(CELL, allocator="tcmalloc", id="chase/x/tcmalloc")
        camp = campaign([cell])
        record = chase_record(cell, allocator="tcmalloc", env={
            "GLIBC_TUNABLES": "glibc.malloc.hugetlb=1:glibc.pthread.rseq=0"})
        self.assertEqual(summary.rejections(record, camp, {cell["id"]: cell}), [])
        record["allocatorEnv"] = {"GLIBC_TUNABLES": "glibc.malloc.hugetlb=1"}
        self.assertTrue(summary.rejections(record, camp, {cell["id"]: cell}))

    def test_requirements_can_be_ignored_explicitly(self):
        record = chase_record(CELL)
        record["system"]["thp"] = "madvise"
        camp = campaign([CELL])
        self.assertEqual(summary.rejections(record, camp, {CELL["id"]: CELL},
                                            ignore_requirements=True), [])

    def test_unpinned_campaign_ignores_migration(self):
        record = chase_record(CELL)
        record["region"]["migrated"] = True
        self.assertEqual(self.reasons(record, campaign([CELL], cpu=None)), [])


class Derived(unittest.TestCase):
    def test_churn_heap_and_fragmentation(self):
        mib = 2 ** 20
        # Baseline 1 MiB + 1 MiB of inputs; troughs hold 10 MiB of heap for
        # 2 MiB live; the first cycle (2 bands x 1 phase) is skipped.
        samples = []
        for phase in range(4):
            samples.append({"anonBytes": 20 * mib, "liveBytes": 5 * mib})
            trough = 30 if phase < 2 else 12
            samples.append({"anonBytes": trough * mib, "liveBytes": 2 * mib})
        record = {"workload": "churn", "region": {},
                  "params": {"bands": "16-64,64-256", "band-phases": 1},
                  "memory": {"startup": {"heldAnonKiB": 1024},
                             "inputBytes": mib, "samples": samples},
                  "metrics": {"nsPerOp": 10.0,
                              "phases": [{"band": p % 2} for p in range(4)]}}
        d = summary.derive(record)
        self.assertAlmostEqual(d["troughHeapMiB"], 10.0)
        self.assertAlmostEqual(d["fragmentation"], 5.0)
        self.assertAlmostEqual(d["peakHeapMiB"], 18.0)

    def test_per_op_counters(self):
        d = summary.derive(chase_record(CELL))
        self.assertAlmostEqual(d["llcPerOp"], 0.5)
        self.assertEqual(d["dtlbPerOp"], 0.0)


class Statistics(unittest.TestCase):
    def test_clear_difference_is_better(self):
        a = [10.0, 10.2, 9.9, 10.1, 10.0, 9.8, 10.3, 10.1]
        b = [20.0, 20.4, 19.8, 20.2, 20.1, 19.9, 20.3, 20.0]
        c = summary.compare(a, b)
        self.assertEqual(c["verdict"], "better")
        self.assertAlmostEqual(c["diffPercent"], -50.0, delta=1.0)
        self.assertEqual(summary.compare(b, a)["verdict"], "worse")

    def test_overlapping_samples_are_not_distinguishable(self):
        a = [10, 12, 9, 11, 13, 10, 12, 8]
        b = [11, 9, 12, 10, 13, 11, 8, 12]
        self.assertEqual(summary.compare(a, b)["verdict"],
                         "not distinguishable")

    def test_exact_p_values(self):
        # Complete separation, exactly: 2 / C(8, 4) and 2 / C(10, 5).
        self.assertAlmostEqual(summary.mann_whitney_p([1, 2, 3, 4], [5, 6, 7, 8]),
                               2 / 70)
        self.assertAlmostEqual(
            summary.mann_whitney_p([1, 2, 3, 4, 5], [6, 7, 8, 9, 10]), 2 / 252)
        # U = 1 for [1, 3]: P(U <= 1) = 2 of 6 orderings, doubled.
        self.assertAlmostEqual(summary.mann_whitney_p([1, 3], [2, 4]), 2 / 3)

    def test_too_few_runs(self):
        # 4 per side can't reach p < 0.01 even when perfectly separated.
        self.assertGreater(summary.mann_whitney_p([1, 2, 3, 4], [5, 6, 7, 8]),
                           summary.ALPHA)
        self.assertEqual(summary.compare([1.0, 2, 3, 4], [5.0, 6, 7, 8])["verdict"],
                         "too few runs")
        self.assertEqual(summary.compare([1.0, 2, 3, 4, 5],
                                         [6.0, 7, 8, 9, 10])["verdict"], "better")


class Bimodality(unittest.TestCase):
    def test_glibc_trimming_is_flagged(self):
        # malloc-cost burst-lifo 4096 under glibc, from the first campaign.
        values = [12.9, 13.7, 13.8, 14.0, 222.6, 222.9, 223.4, 223.6, 223.6, 224.7]
        modes = summary.bimodal(values)
        self.assertEqual((modes["low"]["n"], modes["high"]["n"]), (4, 6))

    def test_ordinary_spread_is_not(self):
        # chase same-class-sweep nodes=16384 under tcmalloc: wide, not split.
        self.assertIsNone(summary.bimodal(
            [5.2, 7.1, 7.3, 8.2, 8.4, 9.9, 10.1, 10.2, 11.2, 11.2]))
        self.assertIsNone(summary.bimodal([10.0, 10.1, 10.2, 10.1, 10.0, 9.9]))
        # One outlier is not a mode.
        self.assertIsNone(summary.bimodal([10, 10.1, 10.2, 10.1, 10, 30]))
        self.assertIsNone(summary.bimodal([1.0, 2.0, 100.0]))


class BimodalCells(unittest.TestCase):
    def test_only_compared_metrics_are_flagged(self):
        cells = [dict(CELL, id=f"chase/x/glibc")]
        camp = campaign(cells)
        records = []
        for i, ns in enumerate([10.0, 10.1, 10.2, 30.0, 30.1, 30.2]):
            r = chase_record(cells[0], ns=ns)
            r["run"]["repetition"] = i
            r["region"]["interrupts"] = 1 if i % 2 else 2  # splits, means nothing
            records.append(r)
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "campaign.json").write_text(json.dumps(camp))
            Path(tmp, "records.jsonl").write_text(
                "".join(json.dumps(r) + "\n" for r in records))
            stats = summary.summarize(Path(tmp))["cells"]["chase/x/glibc"]
        self.assertIn("bimodal", stats["nsPerOp"])
        self.assertNotIn("bimodal", stats["interrupts"])


class Runner(unittest.TestCase):
    MATRIX = {
        "configurations": {
            "glibc": {"backend": "plain", "allocator": "glibc"},
            "pools": {"backend": "pools", "allocator": "glibc", "args": ["--hints=on"]}},
        "workloads": [
            {"name": "chase", "variants": [
                {"name": "s", "args": ["--a=1"], "sweep": {"nodes": [1, 2]}}]},
            {"name": "malloc-cost", "binary": "malloc-cost",
             "configurations": ["glibc"],
             "variants": [{"name": "pair", "args": ["--pattern=pair"]}]}]}

    def test_cells_expand_sweeps_and_backends(self):
        cells = bench_run.expand_cells(self.MATRIX)
        ids = [c["id"] for c in cells]
        self.assertEqual(ids, ["chase/s/nodes=1/glibc", "chase/s/nodes=1/pools",
                               "chase/s/nodes=2/glibc", "chase/s/nodes=2/pools",
                               "malloc-cost/pair/glibc"])
        pools = cells[1]
        self.assertEqual(pools["binary"], "chase-pools")
        self.assertEqual(pools["args"], ["--a=1", "--nodes=1", "--hints=on"])
        self.assertEqual(cells[-1]["args"], ["--pattern=pair"])

    def test_git_state_saves_the_patch_when_dirty(self):
        with tempfile.TemporaryDirectory() as tmp:
            state = bench_run.git_state(Path(tmp))
            self.assertTrue(state["commit"])
            if state["dirty"]:
                patch = (Path(tmp) / "git.diff").read_text()
                self.assertTrue(patch)
                self.assertEqual(state["diffSha256"],
                                 __import__("hashlib").sha256(patch.encode()).hexdigest())
            else:
                self.assertNotIn("diffFile", state)

    def test_environment_merges_tunables_and_drops_the_shells(self):
        os.environ["MALLOC_CONF"] = "from-the-shell"
        try:
            manifest = dict(MANIFEST)
            manifest["both"] = {"library": "/lib/x.so",
                                "runEnv": {"GLIBC_TUNABLES": "a=1"},
                                "hugepageEnv": {"GLIBC_TUNABLES": "b=2"}}
            env = bench_run.environment("both", manifest, hugepages=True)
            self.assertEqual(env["GLIBC_TUNABLES"], "a=1:b=2")
            self.assertEqual(env["LD_PRELOAD"], "/lib/x.so")
            self.assertNotIn("MALLOC_CONF", env)
            env = bench_run.environment("both", manifest, hugepages=False)
            self.assertEqual(env["GLIBC_TUNABLES"], "a=1")
        finally:
            del os.environ["MALLOC_CONF"]


class EndToEnd(unittest.TestCase):
    def test_smoke_campaign(self):
        if BUILD is None:
            self.skipTest("needs --build")
        root = HERE.parent
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "campaign"
            run = subprocess.run(
                [sys.executable, str(HERE / "bench_run.py"), "--matrix",
                 str(root / "bench/matrix-smoke.json"), "--build", str(BUILD),
                 "--deps", str(root / "build-deps"), "--out", str(out)],
                capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = subprocess.run(
                [sys.executable, str(HERE / "bench_summary.py"), str(out)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads((out / "summary.json").read_text())
            self.assertEqual(data["rejected"], [])
            self.assertTrue(data["accepted"])
            self.assertIn("## chase", (out / "summary.md").read_text())
            try:
                import matplotlib  # noqa: F401
            except ImportError:
                return
            plot = subprocess.run(
                [sys.executable, str(HERE / "bench_plot.py"), str(out)],
                capture_output=True, text=True)
            self.assertEqual(plot.returncode, 0, plot.stderr)
            pngs = list((out / "plots").glob("*.png"))
            self.assertTrue(pngs)
            self.assertTrue(all(p.stat().st_size > 1000 for p in pngs))


if __name__ == "__main__":
    if "--build" in sys.argv:
        at = sys.argv.index("--build")
        BUILD = Path(sys.argv[at + 1]).resolve()
        del sys.argv[at:at + 2]
    unittest.main()
