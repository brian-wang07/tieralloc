#!/usr/bin/env python3
"""Charts for a campaign, from the same validated records as bench_summary.py.

Writes RESULTS_DIR/plots/*.svg and *.png:
  dots-<workload>-<metric>    every run per cell: dots, median tick, 95% CI
  forest-<kind>               comparisons: % difference with its 95% CI
  churn-rss                   heap RSS at each phase's trough, per schedule
  orderbook-latency-<variant> latency distribution (ECDF) of each median run
  scatter-<workload>-<variant> time against LLC misses, one panel per config
  malloc-cost                 p50 ns/op against size, one panel per pattern
  sweep-<workload>-<group>    the metric against the swept size, with L2/L3

Every configuration keeps one color in every chart; every series is also
labeled directly, so color is never the only cue. Needs matplotlib; the
summary does not. Usage: scripts/bench_plot.py RESULTS_DIR
"""

import argparse
import random
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_summary  # noqa: E402

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    plt = None

# Fixed order, validated for color-vision deficiency and contrast as a set;
# assigned by configuration, never by rank, so a color means the same
# configuration in every chart. Slots 7-8 are kept for tieralloc.
COLORS = {
    "glibc": "#2a78d6", "jemalloc": "#eb6834", "mimalloc": "#1baf7a",
    "tcmalloc": "#eda100", "pools-hints-off": "#e87ba4",
    "pools-hints-on": "#008300", "tieralloc-hints-off": "#6250d6",
    "tieralloc-hints-on": "#e34948",
}
SPARE = "#8a8984"
INK = "#0b0b0b"
MUTED = "#52514e"
GRID = "#e4e3df"
SURFACE = "#fcfcfb"

# Per workload: the same metric name means different things (p50Ns is per
# message in orderbook, per allocation in malloc-cost).
LABELS = {(workload, metric): label
          for workload, metrics in bench_summary.METRICS.items()
          for metric, label in metrics}


def label(workload, metric):
    return LABELS.get((workload, metric), metric)


def color(config):
    return COLORS.get(config, SPARE)


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE, "axes.edgecolor": GRID,
        "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED,
        "text.color": INK, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.8, "axes.axisbelow": True,
        "axes.spines.top": False, "axes.spines.right": False,
        "font.size": 9, "axes.titlesize": 10, "figure.titlesize": 11,
        "lines.linewidth": 2, "lines.markersize": 5,
        "figure.constrained_layout.use": True,
    })


def label_ends(ax, items, fontsize=7):
    """Direct labels at line ends, spread vertically so close lines don't print
    on top of each other, and kept inside the axes. Call once the y range is
    final (after every shared panel is drawn). items: [(x, y, text, color)]."""
    if not items:
        return
    low, high = ax.get_ylim()
    gap = (high - low) * 0.05
    items = sorted(items, key=lambda i: i[1])
    # Upward pass: each label at least a gap above the one below it.
    placed = []
    for _, y, _, _ in items:
        placed.append(max(y, placed[-1] + gap) if placed else y)
    # If that ran past the top, a downward pass from the top keeps every label
    # as close to its own line as the gaps allow (shifting the whole column
    # would move labels next to other lines).
    top = high - gap / 2
    if placed[-1] > top:
        placed[-1] = top
        for i in range(len(placed) - 2, -1, -1):
            placed[i] = min(placed[i], placed[i + 1] - gap)
    for (x, _, text, ink), y in zip(items, placed):
        ax.annotate(text, (x, y), xytext=(4, 0), textcoords="offset points",
                    va="center", fontsize=fontsize, color=ink,
                    annotation_clip=False)


def legend(target, configs, extra=()):
    """A legend in the fixed configuration order (plus extra (label, handle)
    pairs): identity never depends on color alone or on line position."""
    from matplotlib.lines import Line2D
    handles = [Line2D([], [], color=color(c), lw=2) for c in configs]
    labels = list(configs)
    for label, handle in extra:
        handles.append(handle)
        labels.append(label)
    kwargs = dict(fontsize=7, frameon=False)
    if hasattr(target, "suptitle"):
        target.legend(handles, labels, loc="outside lower center",
                      ncols=min(len(labels), 7), **kwargs)
    else:
        target.legend(handles, labels, loc="best", **kwargs)


def save(fig, out, name):
    out.mkdir(parents=True, exist_ok=True)
    for ext in ("svg", "png"):
        fig.savefig(out / f"{name}.{ext}", dpi=150, bbox_inches="tight")
    plt.close(fig)
    return name


def median_ci(values, seed=1, samples=2000):
    rng = random.Random(seed)
    meds = sorted(statistics.median(rng.choices(values, k=len(values)))
                  for _ in range(samples))
    return meds[int(0.025 * samples)], meds[int(0.975 * samples) - 1]


class Campaign:
    def __init__(self, results, ignore_requirements=False):
        self.summary = bench_summary.summarize(results, ignore_requirements)
        self.meta = self.summary["campaign"]
        self.cells = {c["id"]: c for c in self.meta["cells"]}
        self.runs = defaultdict(list)
        for r in self.summary["accepted"]:
            self.runs[r["run"]["cell"]].append(r["derived"])
        self.configs = [c for c in self.meta["matrix"]["configurations"]]

    def variants(self, workload, sweeps=False):
        seen = []
        for c in self.cells.values():
            if c["workload"] == workload and (c["sweep"] is not None) == sweeps \
                    and c["variant"] not in seen:
                seen.append(c["variant"])
        return seen

    def values(self, workload, variant, config, metric):
        runs = self.runs.get(f"{workload}/{variant}/{config}", [])
        return [r[metric] for r in runs if r.get(metric) is not None]

    def present(self, workload):
        return [c for c in self.configs
                if any(cell["workload"] == workload and
                       cell["configuration"] == c for cell in self.cells.values())]

    def subtitle(self):
        text = f"{self.meta['runs']} runs per cell, campaign {self.meta['id']}"
        if self.meta.get("requirementsIgnored"):
            text += " — NOT A VALID RESULT (requirements ignored)"
        return text

    def has(self, workload, metric):
        return any(r.get(metric) is not None
                   for cell_id, runs in self.runs.items()
                   if self.cells[cell_id]["workload"] == workload for r in runs)


def dots(camp, out):
    """Every run as a dot, the median as a tick, a 95% CI as a bar."""
    made = []
    for workload, metrics in bench_summary.COMPARED.items():
        if workload == "malloc-cost":
            continue  # one cell per size; its own line chart reads better
        variants = camp.variants(workload)
        configs = camp.present(workload)
        if not variants or not configs:
            continue
        for metric in metrics:
            if not camp.has(workload, metric):
                continue
            fig, axes = plt.subplots(1, len(variants), squeeze=False,
                                     figsize=(3.4 * len(variants), 0.45 * len(configs) + 1.4),
                                     sharey=True)
            for ax, variant in zip(axes[0], variants):
                for row, config in enumerate(configs):
                    v = camp.values(workload, variant, config, metric)
                    if not v:
                        continue
                    rng = random.Random(row)
                    jitter = [row + rng.uniform(-0.12, 0.12) for _ in v]
                    ax.scatter(v, jitter, s=22, color=color(config), alpha=0.75,
                               edgecolors=SURFACE, linewidths=0.8, zorder=3)
                    med = statistics.median(v)
                    if len(v) >= 3:
                        lo, hi = median_ci(v)
                        ax.plot([lo, hi], [row - 0.32] * 2, color=INK, lw=1.2,
                                zorder=4)
                    ax.plot([med, med], [row - 0.3, row + 0.3], color=INK, lw=2,
                            zorder=4)
                ax.set_title(variant)
                ax.set_xlabel(label(workload, metric))
                ax.set_yticks(range(len(configs)), configs)
                ax.grid(axis="y", visible=False)
            # The panels share y, so inverting one inverts them all.
            axes[0][0].invert_yaxis()
            fig.suptitle(f"{workload}: {label(workload, metric)} — dots are "
                         f"runs, tick the median, bar its 95% CI\n{camp.subtitle()}",
                         ha="left", x=0.01)
            made.append(save(fig, out, f"dots-{workload}-{metric}"))
    return made


def forest(camp, out):
    """The ablation in one chart; the context comparisons one per workload."""
    made = []
    comparisons = [c for c in camp.summary["comparisons"] if "diffPercent" in c]
    groups = [("ablation", None)] + [
        ("context", w) for w in dict.fromkeys(c["workload"] for c in comparisons
                                              if c["kind"] == "context")]
    for kind, workload in groups:
        rows = [c for c in comparisons if c["kind"] == kind and
                (workload is None or c["workload"] == workload)]
        if not rows:
            continue
        fig, ax = plt.subplots(figsize=(8, 0.3 * len(rows) + 1.6))
        labels = []
        for i, c in enumerate(rows):
            # A context row takes the reference's color: what the subject is
            # compared against is the row's identity.
            ax.plot(c["ci"], [i, i], color=MUTED, lw=1.5, zorder=2)
            ax.scatter([c["diffPercent"]], [i], s=36, zorder=3,
                       color=color(c["reference"]) if kind == "context" else INK,
                       edgecolors=SURFACE, linewidths=1)
            metric = label(c["workload"], c["metric"])
            labels.append((f"{c['variant']} · {metric}"
                           if workload else
                           f"{c['workload']} {c['variant']} · {metric}")
                          + (f" · vs {c['reference']}" if kind == "context" else ""))
            ax.annotate(c["verdict"], (1.01, i), xycoords=("axes fraction", "data"),
                        va="center", fontsize=8,
                        color=INK if c["verdict"] in ("better", "worse") else MUTED)
        ax.axvline(0, color=INK, lw=1)
        ax.set_yticks(range(len(rows)), labels)
        ax.invert_yaxis()
        ax.grid(axis="y", visible=False)
        ax.set_xlabel("% difference of the median (negative = better)")
        subject = rows[0]["subject"]
        title = (f"Hint ablation: {subject} vs {rows[0]['reference']}"
                 if kind == "ablation" else f"{workload}: {subject} against each baseline")
        ax.set_title(f"{title} — point: median difference, line: 95% CI\n"
                     f"{camp.subtitle()}", loc="left")
        made.append(save(fig, out, f"forest-{kind}" +
                         (f"-{workload}" if workload else "")))
    return made


def churn_rss(camp, out):
    variants = camp.variants("churn")
    configs = camp.present("churn")
    if not variants or not camp.has("churn", "series"):
        return []
    fig, axes = plt.subplots(1, len(variants), squeeze=False, sharey=True,
                             figsize=(4.2 * len(variants), 3.6))
    pending = []
    for ax, variant in zip(axes[0], variants):
        bands = None
        live_drawn = False
        ends = []
        for config in configs:
            runs = [r for r in camp.runs.get(f"churn/{variant}/{config}", [])
                    if "series" in r]
            if not runs:
                continue
            series = [r["series"]["troughHeapMiB"] for r in runs]
            phases = range(min(len(s) for s in series))
            med = [statistics.median(s[p] for s in series) for p in phases]
            ax.plot(phases, med, color=color(config), lw=1.8)
            if len(series) >= 2:
                q = [statistics.quantiles([s[p] for s in series], n=4,
                                          method="inclusive") for p in phases]
                ax.fill_between(phases, [x[0] for x in q], [x[2] for x in q],
                                color=color(config), alpha=0.15, lw=0)
            ends.append((phases[-1], med[-1], config, INK))
            if not live_drawn:
                live = [statistics.median(r["series"]["troughLiveMiB"][p]
                                          for r in runs) for p in phases]
                ax.plot(phases, live, color=MUTED, lw=1.2, ls="--")
                ends.append((phases[-1], live[-1], "live data", MUTED))
                live_drawn = True
                bands = runs[0]["series"]["band"]
        if bands:
            start = 0
            for p in range(1, len(bands) + 1):
                if p == len(bands) or bands[p] != bands[start]:
                    if bands[start] % 2:
                        ax.axvspan(start - 0.5, p - 0.5, color=GRID, alpha=0.5,
                                   lw=0, zorder=0)
                    start = p
        ax.set_title(variant)
        ax.set_xlabel("phase (shading: size band changes)")
        ax.margins(x=0.02)
        pending.append((ax, ends))
    for ax, ends in pending:
        label_ends(ax, ends)
    from matplotlib.lines import Line2D
    legend(fig, configs, [("live data", Line2D([], [], color=MUTED, lw=1.2,
                                                 ls="--"))])
    axes[0][0].set_ylabel("heap RSS after the phase's frees (MiB)")
    fig.suptitle("churn: heap RSS at each phase's trough — median across runs, "
                 f"band IQR\n{camp.subtitle()}", x=0.01, ha="left")
    return [save(fig, out, "churn-rss")]


def orderbook_latency(camp, out):
    made = []
    for variant in camp.variants("orderbook"):
        fig, ax = plt.subplots(figsize=(6.5, 3.8))
        drawn = False
        tail = 0.0
        shown = []
        for config in camp.present("orderbook"):
            runs = [r for r in camp.runs.get(f"orderbook/{variant}/{config}", [])
                    if (r.get("series") or {}).get("latencyHistogram")]
            if not runs:
                continue
            runs.sort(key=lambda r: r["p50Ns"])
            hist = runs[len(runs) // 2]["series"]["latencyHistogram"]
            per = hist["bucketsPerOctave"]
            total = sum(hist["counts"])
            xs, ys, seen = [], [], 0
            for i, count in enumerate(hist["counts"]):
                seen += count
                xs.append(2 ** ((hist["firstBucket"] + i + 1) / per))
                ys.append(seen / total)
            ax.step(xs, ys, where="post", color=color(config), lw=1.6)
            tail = max(tail, next(x for x, y in zip(xs, ys) if y >= 0.999))
            shown.append(config)
            drawn = True
        if not drawn:
            plt.close(fig)
            continue
        ax.set_xscale("log")
        # A few outliers would otherwise stretch the axis by orders of
        # magnitude; the record's p99 and max still report them.
        ax.set_xlim(right=tail * 1.5)
        legend(ax, shown)
        ax.set_xlabel("ns per message (log scale, to the 99.9th percentile)")
        ax.set_ylabel("fraction of messages at or below")
        ax.axhline(0.99, color=MUTED, lw=0.8, ls=":")
        ax.set_title(f"orderbook {variant}: latency distribution of each "
                     f"configuration's median run\n{camp.subtitle()}", loc="left")
        made.append(save(fig, out, f"orderbook-latency-{variant}"))
    return made


def scatter(camp, out):
    """Time against LLC misses per op: off the line points to interference."""
    made = []
    for workload, metric in (("chase", "nsPerOp"), ("orderbook", "p50Ns")):
        configs = camp.present(workload)
        for variant in camp.variants(workload):
            pairs = {c: [(r["llcPerOp"], r[metric])
                         for r in camp.runs.get(f"{workload}/{variant}/{c}", [])
                         if r.get("llcPerOp") is not None]
                     for c in configs}
            if not any(pairs.values()):
                continue
            cols = min(3, len(configs))
            rows = (len(configs) + cols - 1) // cols
            fig, axes = plt.subplots(rows, cols, squeeze=False, sharex=True,
                                     sharey=True, figsize=(3.2 * cols, 2.6 * rows))
            for ax, config in zip(axes.flat, configs):
                pts = pairs[config]
                ax.scatter([p[0] for p in pts], [p[1] for p in pts], s=26,
                           color=color(config), edgecolors=SURFACE, linewidths=0.8)
                ax.set_title(config)
            for ax in axes.flat[len(configs):]:
                ax.set_visible(False)
            for ax in axes[-1]:
                ax.set_xlabel("LLC misses per op")
            for ax in axes[:, 0]:
                ax.set_ylabel(label(workload, metric))
            fig.suptitle(f"{workload} {variant}: time against LLC misses, one "
                         f"dot per run\n{camp.subtitle()}", x=0.01, ha="left")
            made.append(save(fig, out, f"scatter-{workload}-{variant}"))
    return made


def sweep_points(camp, workload, group, config, x_metric):
    points = []
    for cell in camp.cells.values():
        if cell["workload"] != workload or cell["variantGroup"] != group or \
                cell["configuration"] != config or cell["sweep"] is None:
            continue
        runs = camp.runs.get(cell["id"], [])
        if not runs:
            continue
        x = statistics.median(r[x_metric] for r in runs) if x_metric \
            else float(cell["sweep"]["value"])
        points.append((x, [r for r in runs]))
    return sorted(points, key=lambda p: p[0])


def malloc_cost(camp, out):
    groups = list(dict.fromkeys(
        c["variantGroup"] for c in camp.cells.values()
        if c["workload"] == "malloc-cost" and c["sweep"] is not None))
    if not groups or not camp.has("malloc-cost", "p50Ns"):
        return []
    fig, axes = plt.subplots(1, len(groups), squeeze=False, sharey=True,
                             figsize=(3.6 * len(groups), 3.4))
    pending = []
    for ax, group in zip(axes[0], groups):
        ends = []
        for config in camp.present("malloc-cost"):
            pts = sweep_points(camp, "malloc-cost", group, config, None)
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [statistics.median(r["p50Ns"] for r in p[1]) for p in pts]
            ax.plot(xs, ys, color=color(config), marker="o")
            ends.append((xs[-1], ys[-1], config, INK))
        ax.set_xscale("log", base=2)
        ax.set_title(group)
        ax.set_xlabel("size (bytes, log scale)")
        pending.append((ax, ends))
    for ax, ends in pending:
        label_ends(ax, ends)
    legend(fig, camp.present("malloc-cost"))
    axes[0][0].set_ylabel("p50 ns per alloc+free (median of runs)")
    fig.suptitle(f"malloc-cost: raw allocation cost by size\n{camp.subtitle()}",
                 x=0.01, ha="left")
    return [save(fig, out, "malloc-cost")]


def sweeps(camp, out):
    made = []
    caches = {c["level"]: c["bytes"] for c in camp.meta["machine"].get("caches", [])
              if c["level"] >= 2}
    for workload in ("chase", "orderbook", "churn"):
        groups = list(dict.fromkeys(
            c["variantGroup"] for c in camp.cells.values()
            if c["workload"] == workload and c["sweep"] is not None))
        metric = bench_summary.COMPARED[workload][0]
        x_metric = "hotSetBytes" if workload == "chase" else None
        if not camp.has(workload, metric):
            continue
        for group in groups:
            fig, ax = plt.subplots(figsize=(6.5, 3.8))
            ends = []
            for config in camp.present(workload):
                pts = sweep_points(camp, workload, group, config, x_metric)
                pts = [(x, [r[metric] for r in runs if r.get(metric) is not None])
                       for x, runs in pts]
                pts = [(x, v) for x, v in pts if v]
                if not pts:
                    continue
                xs = [p[0] for p in pts]
                med = [statistics.median(p[1]) for p in pts]
                ax.plot(xs, med, color=color(config), marker="o")
                if all(len(p[1]) >= 2 for p in pts):
                    q = [statistics.quantiles(p[1], n=4, method="inclusive")
                         for p in pts]
                    ax.fill_between(xs, [x[0] for x in q], [x[2] for x in q],
                                    color=color(config), alpha=0.15, lw=0)
                ends.append((xs[-1], med[-1], config, INK))
            if x_metric:
                for level, size in sorted(caches.items()):
                    ax.axvline(size, color=MUTED, lw=0.8, ls=":")
                    ax.annotate(f"L{level}", (size, 1.0), xycoords=("data", "axes fraction"),
                                xytext=(2, -10), textcoords="offset points",
                                fontsize=7, color=MUTED)
            ax.set_xscale("log", base=2)
            label_ends(ax, ends)
            legend(ax, [e[2] for e in sorted(ends, key=lambda e: -e[1])])
            ax.set_xlabel("hot set (bytes, log scale)" if x_metric
                          else "swept value (log scale)")
            ax.set_ylabel(label(workload, metric))
            ax.set_title(f"{workload} {group}: median and IQR across runs\n"
                         f"{camp.subtitle()}", loc="left")
            made.append(save(fig, out, f"sweep-{workload}-{group}"))
    return made


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", type=Path)
    parser.add_argument("--ignore-requirements", action="store_true",
                        help="as for bench_summary.py; charts are marked")
    args = parser.parse_args()
    if plt is None:
        print("bench_plot: matplotlib is not installed (pip install matplotlib); "
              "the summary tables do not need it", file=sys.stderr)
        return 2
    style()
    camp = Campaign(args.results, args.ignore_requirements)
    out = args.results / "plots"
    made = []
    for chart in (dots, forest, churn_rss, orderbook_latency, scatter,
                  malloc_cost, sweeps):
        made += chart(camp, out)
    print(f"bench_plot: {len(made)} charts in {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
