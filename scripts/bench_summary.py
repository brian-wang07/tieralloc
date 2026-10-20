#!/usr/bin/env python3
"""Turns a campaign's records into the results tables.

Every record is validated first; a rejected record never reaches a table, and
the reasons are listed. Derived metrics (heap RSS, fragmentation, per-op
counters) are computed here and only here. Per cell: median, IQR and range
across runs. Per comparison (the matrix's "comparisons"): the median
difference with a bootstrap 95% CI and a Mann-Whitney p, and a verdict that is
"better" or "worse" only when the CI excludes zero and p < 0.01.

Writes, into the campaign directory (or --out):
  summary.md    the tables: the M0 results
  summary.csv   one row per cell and metric
  summary.json  validated records' derived metrics, cells and comparisons,
                for bench_plot.py

Standard library only. Usage: scripts/bench_summary.py RESULTS_DIR [--out DIR]
"""

import argparse
import csv
import json
import math
import random
import statistics
import sys
from collections import defaultdict
from pathlib import Path

SCHEMA = 5
ALPHA = 0.01
# Mann-Whitney cannot reach p < ALPHA with fewer runs per side, however clean
# the separation (exactly: 4 vs 4 gives p = 2/70 = 0.029 at best, 5 vs 5 gives
# 2/252 = 0.008), so below this a verdict would always be "not
# distinguishable"; say "too few runs" instead.
MIN_RUNS = 5
# More involuntary context switches than this during a pinned region means
# another task shared the CPU (undisturbed runs have 0-16; disturbed ones
# 560-870 in the first full campaign).
MAX_INVOLUNTARY_SWITCHES = 50
# Up to this many runs in total, without ties, p is computed exactly.
EXACT_LIMIT = 60
BOOTSTRAP_SAMPLES = 20000

# Per workload: the metrics tabled, in order; the first is the headline.
# Every metric is lower-is-better.
METRICS = {
    "churn": [("troughHeapMiB", "trough heap RSS (MiB)"),
              ("fragmentation", "trough RSS / live"),
              ("peakHeapMiB", "peak heap RSS (MiB)"),
              ("nsPerOp", "ns per alloc/free")],
    "orderbook": [("p50Ns", "p50 ns/message"),
                  ("p99Ns", "p99 ns/message"),
                  ("cyclesPerOp", "cycles/message"),
                  ("llcPerOp", "LLC misses/message"),
                  ("dtlbPerOp", "dTLB walks/message"),
                  ("heapMiB", "heap RSS at end (MiB)")],
    "chase": [("nsPerOp", "ns/visit"),
              ("cyclesPerOp", "cycles/visit"),
              ("llcPerOp", "LLC misses/visit"),
              ("dtlbPerOp", "dTLB walks/visit")],
    "malloc-cost": [("p50Ns", "p50 ns/op"),
                    ("cyclesPerOp", "cycles/op"),
                    ("instructionsPerOp", "instructions/op")],
}
# Metrics compared between configurations; the rest are explanatory.
COMPARED = {"churn": ["troughHeapMiB", "nsPerOp"],
            "orderbook": ["p50Ns", "p99Ns"],
            "chase": ["nsPerOp"],
            "malloc-cost": ["p50Ns"]}


# --- validation -------------------------------------------------------------

def required_env(campaign, allocator):
    """The settings a run of allocator must have had: runEnv, and hugepageEnv
    when the campaign uses huge pages. GLIBC_TUNABLES as a set of items."""
    entry = campaign["manifest"][allocator]
    required = {}
    sources = [entry.get("runEnv", {})]
    if campaign.get("hugepages"):
        sources.append(entry.get("hugepageEnv", {}))
    for source in sources:
        for key, value in source.items():
            required.setdefault(key, set()).update(
                value.split(":") if key == "GLIBC_TUNABLES" else [value])
    return required


def rejections(record, campaign, cells, ignore_requirements=False):
    """Why record must not be used; empty if it may. ignore_requirements skips
    the matrix's machine requirements (THP, governor) only."""
    reasons = []
    run = record.get("run")
    if run is None or run.get("cell") not in cells:
        return ["not a run of this campaign"]
    cell = cells[run["cell"]]
    if record.get("schema") != SCHEMA:
        reasons.append(f"schema {record.get('schema')}, expected {SCHEMA}")
    if record["build"]["contractSemantic"] not in ("ignore", "none"):
        reasons.append(f"contract semantic {record['build']['contractSemantic']}")
    if record["allocator"]["name"] != cell["allocator"]:
        reasons.append(f"served by {record['allocator']['name']}")
    env = record.get("allocatorEnv", {})
    for key, values in required_env(campaign, cell["allocator"]).items():
        have = set(env.get(key, "").split(":")) if key == "GLIBC_TUNABLES" \
            else {env.get(key)}
        if not values <= have:
            reasons.append(f"missing {key}={':'.join(sorted(values))}")
    # Only a pinned campaign can migrate by mistake; an unpinned one (the smoke
    # campaign) moves freely.
    if campaign.get("cpu") is not None and record["region"].get("migrated"):
        reasons.append("migrated between CPUs")
    switches = record["region"].get("contextSwitches") or {}
    if campaign.get("cpu") is not None and \
            switches.get("involuntary", 0) > MAX_INVOLUNTARY_SWITCHES:
        reasons.append(f"{switches['involuntary']} involuntary context "
                       "switches (another task shared the CPU)")
    warmup = record["system"]["warmup"]
    if campaign.get("counters") != "off" and warmup.get("converged") is not True:
        reasons.append("CPU warmup did not converge")
    if campaign.get("counters") == "required":
        counters = record["counters"]
        if not counters.get("available") or \
                (counters.get("region") or {}).get("status") != "ok":
            reasons.append("counters missing or not ok")
    seam = record["metrics"].get("seam")
    if seam is not None and seam.get("leakedBytes") != 0:
        reasons.append(f"leaked {seam.get('leakedBytes')} bytes")
    if record["memory"].get("droppedSamples", 0):
        reasons.append("dropped RSS samples")
    require = {} if ignore_requirements else campaign["matrix"].get("require", {})
    if "thp" in require and record["system"].get("thp") != require["thp"]:
        reasons.append(f"THP {record['system'].get('thp')}")
    if "governor" in require and \
            record["system"].get("governor") != require["governor"]:
        reasons.append(f"governor {record['system'].get('governor')}")
    return reasons


# --- derived metrics --------------------------------------------------------

def counter(record, name):
    region = (record["counters"] or {}).get("region") or {}
    if region.get("status") != "ok":
        return None
    return region["values"].get(name)


def per_op(record, name, ops):
    value = counter(record, name)
    return None if value is None or not ops else value / ops


def heap_base(record):
    """Bytes to subtract from RSS: the startup baseline and the inputs the
    workload mapped for itself."""
    memory = record["memory"]
    startup = memory["startup"]
    return (startup["heldAnonKiB"] * 1024 if startup else 0) + memory["inputBytes"]


def end_heap_mib(record):
    after = record["memory"]["after"]
    if not after:
        return None
    return (after["heldAnonKiB"] * 1024 - heap_base(record)) / 2 ** 20


def churn_skip(params):
    """Phases before the heap reaches its steady state: one full cycle of the
    bands, or a survivor lifetime for the single-band control."""
    bands = len(str(params.get("bands", "")).split(","))
    if bands > 1:
        return bands * int(params.get("band-phases", 1))
    return int(params.get("survivor-phases", 1))


def derive(record):
    """{metric: value} plus "series" for plots, by workload."""
    workload = record["workload"]
    m = record["metrics"]
    d = {"interrupts": record["region"].get("interrupts"),
         "involuntarySwitches": (record["region"].get("contextSwitches") or {})
         .get("involuntary")}
    if workload == "churn":
        base = heap_base(record)
        samples = record["memory"]["samples"]
        peaks, troughs = samples[0::2], samples[1::2]
        heap = [(s["anonBytes"] - base) / 2 ** 20 for s in troughs]
        live = [s["liveBytes"] / 2 ** 20 for s in troughs]
        steady = list(range(churn_skip(record["params"]), len(troughs)))
        if steady:
            d["troughHeapMiB"] = statistics.fmean(heap[i] for i in steady)
            d["fragmentation"] = statistics.fmean(
                heap[i] / live[i] for i in steady if live[i] > 0)
        d["peakHeapMiB"] = max((s["anonBytes"] - base) / 2 ** 20 for s in peaks)
        d["nsPerOp"] = m["nsPerOp"]
        d["series"] = {"troughHeapMiB": heap, "troughLiveMiB": live,
                       "band": [p["band"] for p in m["phases"]]}
    elif workload == "orderbook":
        n = m["messages"]
        d["p50Ns"] = m["nsPerMessage"]["p50"]
        d["p99Ns"] = m["nsPerMessage"]["p99"]
        d["cyclesPerOp"] = m["cyclesPerMessage"]
        d["llcPerOp"] = per_op(record, "llcLoadMisses", n)
        d["dtlbPerOp"] = per_op(record, "dtlbLoadWalks", n)
        d["heapMiB"] = end_heap_mib(record)
        d["series"] = {"latencyHistogram": m.get("latencyHistogram")}
    elif workload == "chase":
        n = m["visits"]
        d["nsPerOp"] = m["nsPerVisit"]
        d["cyclesPerOp"] = m["cyclesPerVisit"]
        d["llcPerOp"] = per_op(record, "llcLoadMisses", n)
        d["dtlbPerOp"] = per_op(record, "dtlbLoadWalks", n)
        d["hotSetBytes"] = m["hotSetBytes"]
    elif workload == "malloc-cost":
        n = m["ops"]
        d["p50Ns"] = m["nsPerOp"]["p50"]
        d["cyclesPerOp"] = m["cyclesPerOp"]
        d["instructionsPerOp"] = per_op(record, "instructions", n)
        d["llcPerOp"] = per_op(record, "llcLoadMisses", n)
    return d


# --- statistics -------------------------------------------------------------

def describe(values):
    values = sorted(values)
    n = len(values)
    if n >= 2:
        q1, _, q3 = statistics.quantiles(values, n=4, method="inclusive")
    else:
        q1 = q3 = values[0]
    return {"n": n, "median": statistics.median(values), "q1": q1, "q3": q3,
            "min": values[0], "max": values[-1]}


def bimodal(values):
    """The two modes if values split into two clearly separated groups, else
    None. A median then describes neither group (e.g. glibc trimming its heap
    in some runs and not others), so the summary flags such cells.

    Split at the largest gap between sorted values; bimodal if each side has
    at least two runs and the gap is both large (a fifth of the median) and
    much wider than either group's own range."""
    values = sorted(values)
    if len(values) < 6:
        return None
    gaps = [values[i + 1] - values[i] for i in range(len(values) - 1)]
    at = max(range(len(gaps)), key=gaps.__getitem__)
    low, high = values[:at + 1], values[at + 1:]
    if len(low) < 2 or len(high) < 2:
        return None
    gap = gaps[at]
    spread = max(low[-1] - low[0], high[-1] - high[0])
    if gap < 0.2 * statistics.median(values) or gap < 3 * spread:
        return None
    return {"low": {"n": len(low), "median": statistics.median(low)},
            "high": {"n": len(high), "median": statistics.median(high)}}


def exact_u_counts(m, n):
    """counts[u]: how many of the C(m+n, m) orderings give U = u."""
    # f(u; m, n) = f(u - n; m - 1, n) + f(u; m, n - 1), built up row by row.
    table = {}
    for i in range(m + 1):
        for j in range(n + 1):
            if i == 0 or j == 0:
                table[i, j] = [1]
                continue
            left, below = table[i - 1, j], table[i, j - 1]
            counts = [0] * (i * j + 1)
            for u, c in enumerate(below):
                counts[u] += c
            for u, c in enumerate(left):
                counts[u + j] += c
            table[i, j] = counts
    return table[m, n]


def mann_whitney_p(a, b):
    """Two-sided p. Exact when there are no ties and few runs; otherwise the
    normal approximation with tie and continuity correction."""
    pooled = sorted((v, i) for i, v in enumerate(a + b))
    ranks = [0.0] * len(pooled)
    i = 0
    ties = 0.0
    while i < len(pooled):
        j = i
        while j + 1 < len(pooled) and pooled[j + 1][0] == pooled[i][0]:
            j += 1
        for k in range(i, j + 1):
            ranks[pooled[k][1]] = (i + j) / 2 + 1
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    n1, n2 = len(a), len(b)
    n = n1 + n2
    u = sum(ranks[:n1]) - n1 * (n1 + 1) / 2
    if ties == 0 and n <= EXACT_LIMIT:
        counts = exact_u_counts(n1, n2)
        total = sum(counts)
        extreme = min(u, n1 * n2 - u)
        tail = sum(counts[:int(extreme) + 1]) / total
        return min(1.0, 2 * tail)
    variance = n1 * n2 / 12 * ((n + 1) - ties / (n * (n - 1)))
    if variance <= 0:
        return 1.0
    z = max(abs(u - n1 * n2 / 2) - 0.5, 0) / math.sqrt(variance)
    return math.erfc(z / math.sqrt(2))


def bootstrap_ci(a, b, seed=1):
    """95% CI of the % difference of a's median from b's."""
    rng = random.Random(seed)
    diffs = []
    for _ in range(BOOTSTRAP_SAMPLES):
        ma = statistics.median(rng.choices(a, k=len(a)))
        mb = statistics.median(rng.choices(b, k=len(b)))
        diffs.append((ma - mb) / mb * 100 if mb else 0.0)
    diffs.sort()
    return (diffs[int(0.025 * BOOTSTRAP_SAMPLES)],
            diffs[int(0.975 * BOOTSTRAP_SAMPLES) - 1])


def compare(a, b):
    """subject a vs reference b, lower is better."""
    if len(a) < MIN_RUNS or len(b) < MIN_RUNS:
        return {"verdict": "too few runs", "n": [len(a), len(b)]}
    mb = statistics.median(b)
    diff = (statistics.median(a) - mb) / mb * 100 if mb else 0.0
    lo, hi = bootstrap_ci(a, b)
    p = mann_whitney_p(a, b)
    if p < ALPHA and hi < 0:
        verdict = "better"
    elif p < ALPHA and lo > 0:
        verdict = "worse"
    else:
        verdict = "not distinguishable"
    return {"diffPercent": diff, "ci": [lo, hi], "p": p, "verdict": verdict,
            "n": [len(a), len(b)]}


# --- the campaign -----------------------------------------------------------

def load(results):
    campaign = json.loads((results / "campaign.json").read_text())
    records = [json.loads(line) for line in
               (results / "records.jsonl").read_text().splitlines() if line.strip()]
    return campaign, records


def natural(key):
    """Sort key that orders numbers by value: nodes=16384 before 1048576."""
    import re
    return [[int(t) if t.isdigit() else t for t in re.split(r"(\d+)", part)]
            for part in key]


def summarize(results, ignore_requirements=False):
    """Everything the outputs (and bench_plot.py) need."""
    campaign, records = load(results)
    campaign["requirementsIgnored"] = ignore_requirements
    cells = {c["id"]: c for c in campaign["cells"]}
    accepted, rejected = [], []
    for record in records:
        reasons = rejections(record, campaign, cells, ignore_requirements)
        if reasons:
            rejected.append({"run": record.get("run"), "reasons": reasons})
        else:
            accepted.append({"run": record["run"], "derived": derive(record)})

    by_cell = defaultdict(list)
    for r in accepted:
        by_cell[r["run"]["cell"]].append(r)
    cell_stats = {}
    for cell_id, runs in by_cell.items():
        workload = cells[cell_id]["workload"]
        stats = {}
        for metric, _ in METRICS.get(workload, []) + [("interrupts", "")]:
            values = [r["derived"].get(metric) for r in runs]
            values = [v for v in values if v is not None]
            if values:
                stats[metric] = describe(values)
                # Only the measured quantities: counts such as interrupts (1 or
                # 2 timer ticks) and near-zero rates split without meaning.
                modes = bimodal(values) if metric in COMPARED.get(workload, []) \
                    else None
                if modes:
                    stats[metric]["bimodal"] = modes
        cell_stats[cell_id] = stats

    comparisons = []
    spec = campaign["matrix"].get("comparisons", {})
    pairs = [("ablation", s, ref) for s, ref in spec.get("ablation", [])]
    context = spec.get("context")
    if context:
        pairs += [("context", context["subject"], ref) for ref in context["against"]]
    groups = sorted({(c["workload"], c["variant"]) for c in cells.values()},
                    key=natural)
    for workload, variant in groups:
        for kind, subject, reference in pairs:
            a_id = f"{workload}/{variant}/{subject}"
            b_id = f"{workload}/{variant}/{reference}"
            if a_id not in by_cell or b_id not in by_cell:
                continue
            for metric in COMPARED.get(workload, []):
                a = [r["derived"][metric] for r in by_cell[a_id]
                     if r["derived"].get(metric) is not None]
                b = [r["derived"][metric] for r in by_cell[b_id]
                     if r["derived"].get(metric) is not None]
                if a and b:
                    split = any("bimodal" in cell_stats[c].get(metric, {})
                                for c in (a_id, b_id))
                    comparisons.append({"kind": kind, "workload": workload,
                                        "variant": variant, "metric": metric,
                                        "subject": subject,
                                        "reference": reference,
                                        "bimodal": split, **compare(a, b)})
    return {"campaign": campaign, "accepted": accepted, "rejected": rejected,
            "cells": cell_stats, "comparisons": comparisons}


def fmt(value, digits=2):
    if value is None:
        return "–"
    if abs(value) >= 100:
        return f"{value:.0f}"
    if abs(value) >= 10:
        return f"{value:.1f}"
    return f"{value:.{digits}f}"


def cell_text(stat):
    if not stat:
        return "–"
    spread = stat["q3"] - stat["q1"]
    relative = spread / stat["median"] * 100 if stat["median"] else 0
    mark = " ‡" if "bimodal" in stat else ""
    return f"{fmt(stat['median'])} ±{relative:.0f}%{mark}"


def markdown(summary):
    campaign = summary["campaign"]
    cells = {c["id"]: c for c in campaign["cells"]}
    machine = campaign["machine"]
    git = campaign["git"]
    lines = [f"# Results: campaign {campaign['id']}", ""]
    lines.append(f"- **Machine:** {machine['cpuModel']}, kernel {machine['kernel']}, "
                 f"CPU {campaign['cpu']}")
    if git.get("diffFile"):
        code = (f"{git['commit']} plus the uncommitted changes in "
                f"{git['diffFile']} (sha256 {git['diffSha256'][:12]})")
    else:
        code = git["commit"] + (" (with uncommitted changes, not saved)"
                                if git["dirty"] else "")
    lines.append(f"- **Code:** {code}")
    for resume in campaign.get("resumes", []):
        if isinstance(resume, dict) and not resume.get("sameCode", True):
            lines.append(f"- **Resumed at {resume['time']} with different "
                         "code**, so the runs mix two versions")
    lines.append(f"- **Runs:** {campaign['runs']} per cell, status "
                 f"{campaign['status']}, {len(summary['accepted'])} records "
                 f"used, {len(summary['rejected'])} rejected")
    if campaign.get("requirementsIgnored"):
        lines.append("- **NOT A VALID RESULT: summarized with "
                     "--ignore-requirements**, so records that ran without the "
                     "matrix's THP and governor settings were used")
    if campaign.get("forced"):
        lines.append("- **Forced past preflight errors**, so the machine did "
                     "not meet the matrix's requirements")
    for check in campaign.get("preflight", []):
        if not check["ok"]:
            lines.append(f"- {check['level']}: {check['detail']}")
    lines += ["", "Cells show the median across runs ± the IQR as a % of the "
              "median. Comparisons give the median difference with a bootstrap "
              f"95% CI; a verdict needs the CI to exclude 0 and Mann-Whitney "
              f"p < {ALPHA}. Lower is better everywhere. ‡ marks a bimodal "
              "cell, whose median describes neither of its modes (see the "
              "list at the end).", ""]

    for workload, metrics in METRICS.items():
        ids = [c for c in cells if cells[c]["workload"] == workload]
        if not ids:
            continue
        lines += [f"## {workload}", ""]
        variants = list(dict.fromkeys(cells[c]["variant"] for c in ids))
        configs = list(dict.fromkeys(cells[c]["configuration"] for c in ids))
        for variant in variants:
            lines += [f"### {variant}", "",
                      "| configuration | " + " | ".join(m[1] for m in metrics) +
                      " | interrupts |",
                      "|---|" + "---|" * (len(metrics) + 1)]
            for config in configs:
                stats = summary["cells"].get(f"{workload}/{variant}/{config}")
                if stats is None:
                    continue
                row = [cell_text(stats.get(m[0])) for m in metrics]
                irq = stats.get("interrupts")
                row.append(fmt(irq["median"]) if irq else "–")
                lines.append(f"| {config} | " + " | ".join(row) + " |")
            lines.append("")
            rows = [c for c in summary["comparisons"]
                    if c["workload"] == workload and c["variant"] == variant]
            if rows:
                lines += ["| comparison | metric | difference | 95% CI | p | verdict |",
                          "|---|---|---|---|---|---|"]
                for c in rows:
                    if "diffPercent" not in c:
                        lines.append(f"| {c['subject']} vs {c['reference']} | "
                                     f"{c['metric']} | – | – | – | {c['verdict']} |")
                        continue
                    lines.append(
                        f"| {c['subject']} vs {c['reference']} ({c['kind']}) | "
                        f"{c['metric']} | {c['diffPercent']:+.1f}% | "
                        f"[{c['ci'][0]:+.1f}%, {c['ci'][1]:+.1f}%] | "
                        f"{c['p']:.1e} | **{c['verdict']}**"
                        + (" (bimodal cell)" if c.get("bimodal") else "") + " |")
                lines.append("")
    split = [(cell_id, metric, stat["bimodal"])
             for cell_id, stats in summary["cells"].items()
             for metric, stat in stats.items() if "bimodal" in stat]
    if split:
        lines += ["## Bimodal cells", "",
                  "Runs of these cells fall into two separate groups, so their "
                  "median describes neither; report both modes. Look at the "
                  "dot plots, and at what differs between the groups' runs.",
                  "", "| cell | metric | low mode | high mode |",
                  "|---|---|---|---|"]
        for cell_id, metric, modes in split:
            lines.append(f"| {cell_id} | {metric} | {fmt(modes['low']['median'])} "
                         f"({modes['low']['n']} runs) | "
                         f"{fmt(modes['high']['median'])} "
                         f"({modes['high']['n']} runs) |")
        lines.append("")
    if summary["rejected"]:
        lines += ["## Rejected records", "", "| cell | run | reasons |",
                  "|---|---|---|"]
        for r in summary["rejected"]:
            run = r["run"] or {}
            lines.append(f"| {run.get('cell', '?')} | {run.get('repetition', '?')} "
                         f"| {'; '.join(r['reasons'])} |")
        lines.append("")
    return "\n".join(lines)


def write_csv(summary, path):
    cells = {c["id"]: c for c in summary["campaign"]["cells"]}
    with path.open("w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["workload", "variant", "configuration", "metric", "n",
                      "median", "q1", "q3", "min", "max"])
        for cell_id, stats in summary["cells"].items():
            cell = cells[cell_id]
            for metric, s in stats.items():
                out.writerow([cell["workload"], cell["variant"],
                              cell["configuration"], metric, s["n"], s["median"],
                              s["q1"], s["q3"], s["min"], s["max"]])


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--ignore-requirements", action="store_true",
                        help="use records that ran without the matrix's THP "
                             "and governor settings (marked as not valid)")
    args = parser.parse_args()
    try:
        summary = summarize(args.results, args.ignore_requirements)
    except (OSError, json.JSONDecodeError, KeyError) as e:
        print(f"bench_summary: {e!r}", file=sys.stderr)
        return 2
    out = args.out or args.results
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.md").write_text(markdown(summary) + "\n")
    write_csv(summary, out / "summary.csv")
    (out / "summary.json").write_text(json.dumps(
        {k: summary[k] for k in ("accepted", "rejected", "cells", "comparisons")}
        | {"campaignId": summary["campaign"]["id"]}) + "\n")
    print(f"bench_summary: {len(summary['accepted'])} records used, "
          f"{len(summary['rejected'])} rejected; {out / 'summary.md'}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
