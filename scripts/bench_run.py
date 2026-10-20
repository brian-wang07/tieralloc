#!/usr/bin/env python3
"""Runs a benchmark campaign: every cell of a matrix, N times, one process each.

A cell is (workload, variant, configuration). The runner checks the machine
first and refuses to start if it doesn't match the matrix's requirements (it
never changes system settings itself; --force runs anyway and records that).
Runs are shuffled with the matrix's seed, so drift and background activity
spread over every cell instead of landing on whichever ran last. Each run gets
its allocator's LD_PRELOAD, runEnv and (with "hugepages") hugepageEnv from the
manifest, with GLIBC_TUNABLES values joined by ':'.

Output, in --out:
  campaign.json   the matrix, git commit, machine, preflight, status
  records.jsonl   one record per run, with a "run" field naming its cell
  logs/           stderr of every failed run

A campaign is resumable: rerunning with the same --out skips the runs that
already have a record. Exit codes: 0 done, 1 aborted, 2 bad usage or matrix.

Usage: scripts/bench_run.py [--matrix bench/matrix.json] [--out DIR]
         [--build build] [--deps build-deps] [--runs N] [--cpu N]
         [--only WORKLOAD,...] [--dry-run] [--force]
"""

import argparse
import datetime
import hashlib
import itertools
import json
import os
import platform
import random
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Allocator tuning variables a run must not inherit from the shell.
ALLOCATOR_ENV_PREFIXES = ("LD_PRELOAD", "GLIBC_TUNABLES", "MALLOC_CONF",
                          "MIMALLOC_", "TCMALLOC_")
# Stop after this many failed runs in a row: something systematic is wrong.
MAX_CONSECUTIVE_FAILURES = 5


class MatrixError(Exception):
    pass


def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def selected_mode(path):
    """The bracketed choice in a sysfs mode file, e.g. 'always'."""
    text = read_text(path)
    if text and "[" in text and "]" in text:
        return text[text.index("[") + 1:text.index("]")]
    return None


def expand_cells(matrix, only=None):
    """Every (workload, variant, configuration) cell, sweeps expanded."""
    configurations = matrix["configurations"]
    cells = []
    for workload in matrix["workloads"]:
        name = workload["name"]
        if only and name not in only:
            continue
        names = workload.get("configurations", list(configurations))
        for config_name in names:
            if config_name not in configurations:
                raise MatrixError(f"{name}: unknown configuration {config_name}")
        for variant in workload["variants"]:
            points = [(None, None, [])]
            if "sweep" in variant:
                if len(variant["sweep"]) != 1:
                    raise MatrixError(f"{name}/{variant['name']}: sweep one flag")
                [(flag, values)] = variant["sweep"].items()
                points = [(flag, v, [f"--{flag}={v}"]) for v in values]
            for flag, value, extra in points:
                variant_name = variant["name"]
                if flag is not None:
                    variant_name += f"/{flag}={value}"
                for config_name in names:
                    config = configurations[config_name]
                    binary = workload.get("binary",
                                          f"{name}-{config['backend']}")
                    is_micro = "binary" in workload
                    args = list(variant.get("args", [])) + extra
                    if not is_micro:
                        args += config.get("args", [])
                    cells.append({
                        "id": f"{name}/{variant_name}/{config_name}",
                        "workload": name,
                        "variant": variant_name,
                        "variantGroup": variant["name"],
                        "sweep": None if flag is None else
                                 {"flag": flag, "value": value},
                        "configuration": config_name,
                        "backend": config["backend"],
                        "allocator": config["allocator"],
                        "binary": binary,
                        "args": args,
                    })
    return cells


def environment(allocator, manifest, hugepages):
    """The run's environment: the shell's, minus allocator tuning, plus the
    manifest's settings for this allocator."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(ALLOCATOR_ENV_PREFIXES)}
    entry = manifest[allocator]
    sources = [entry.get("runEnv", {})]
    if hugepages:
        sources.append(entry.get("hugepageEnv", {}))
    tunables = []
    for source in sources:
        for key, value in source.items():
            if key == "GLIBC_TUNABLES":
                tunables.append(value)
            else:
                env[key] = value
    if tunables:
        env["GLIBC_TUNABLES"] = ":".join(tunables)
    if entry.get("library"):
        env["LD_PRELOAD"] = entry["library"]
    return env


def caches(cpu):
    """The cache hierarchy of cpu, from sysfs: [{level, type, bytes}]."""
    found = []
    base = Path(f"/sys/devices/system/cpu/cpu{cpu if cpu is not None else 0}/cache")
    for index in sorted(base.glob("index*")):
        size = read_text(index / "size") or ""
        multiplier = {"K": 1024, "M": 1024 ** 2}.get(size[-1:], 1)
        digits = size.rstrip("KM")
        if not digits.isdigit():
            continue
        found.append({"level": int(read_text(index / "level") or 0),
                      "type": read_text(index / "type"),
                      "bytes": int(digits) * multiplier})
    return found


def machine(cpu):
    model = None
    for line in (read_text("/proc/cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    meminfo = read_text("/proc/meminfo") or ""
    total = next((int(l.split()[1]) * 1024 for l in meminfo.splitlines()
                  if l.startswith("MemTotal:")), None)
    return {"cpuModel": model, "kernel": platform.release(),
            "cpus": os.cpu_count(), "memoryBytes": total,
            "caches": caches(cpu), "python": platform.python_version()}


def git(*args, ok=(0,)):
    result = subprocess.run(["git", "-C", str(ROOT), *args],
                            capture_output=True, text=True)
    return result.stdout if result.returncode in ok else None


def git_diff():
    """Every uncommitted change, untracked files included, as one patch that
    applies to the commit: with it, a dirty campaign's code is still exactly
    reproducible. Empty for a clean tree, None outside a git checkout."""
    tracked = git("diff", "HEAD", "--binary")
    if tracked is None:
        return None
    untracked = git("ls-files", "--others", "--exclude-standard", "-z") or ""
    parts = [tracked]
    for path in filter(None, untracked.split("\0")):
        # --no-index exits 1 when the files differ, which they always do here.
        parts.append(git("diff", "--no-index", "--binary", "/dev/null", path,
                         ok=(0, 1)) or "")
    return "".join(parts)


def git_state(out):
    """The commit, and when the tree is dirty the patch on top of it, saved as
    out/git.diff and identified by its hash."""
    commit = git("rev-parse", "HEAD")
    diff = git_diff()
    state = {"commit": commit.strip() if commit else None,
             "dirty": None if diff is None else bool(diff)}
    if diff:
        (out / "git.diff").write_text(diff)
        state["diffFile"] = "git.diff"
        state["diffSha256"] = hashlib.sha256(diff.encode()).hexdigest()
    return state


def preflight(matrix, cells, manifest, build, cpu):
    """[{check, level, ok, detail}]; level 'error' blocks a campaign."""
    checks = []

    def add(check, ok, detail, level="error"):
        checks.append({"check": check, "level": level, "ok": bool(ok),
                       "detail": detail})

    require = matrix.get("require", {})
    if "thp" in require:
        thp = selected_mode("/sys/kernel/mm/transparent_hugepage/enabled")
        add("thp", thp == require["thp"],
            f"THP is {thp}, the matrix requires {require['thp']} "
            "(echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled)")
    if cpu is not None:
        cpu_dir = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        add("cpu", cpu_dir.exists(), f"CPU {cpu} exists")
        if "governor" in require:
            governor = read_text(cpu_dir / "cpufreq/scaling_governor")
            add("governor", governor == require["governor"],
                f"CPU {cpu}'s governor is {governor}, the matrix requires "
                f"{require['governor']} (sudo cpupower frequency-set -g "
                f"{require['governor']})")
        isolated = read_text("/sys/devices/system/cpu/isolated") or ""
        add("isolated", str(cpu) in expand_cpu_list(isolated),
            f"CPU {cpu} is not in isolcpus ({isolated or 'none'}); other "
            "tasks and interrupts may run on it", level="warning")
        siblings = read_text(cpu_dir / "topology/thread_siblings_list") or ""
        others = [c for c in expand_cpu_list(siblings) if c != str(cpu)]
        add("smt", not others,
            f"CPU {cpu} shares its core with {','.join(others)}; keep it idle",
            level="warning")
    else:
        add("cpu", False, "runs are not pinned (matrix cpu is null)",
            level="warning")
    load = read_text("/proc/loadavg")
    load1 = float(load.split()[0]) if load else 0.0
    add("load", load1 < 1.0, f"1-minute load average is {load1:.2f}",
        level="warning")

    for binary in sorted({c["binary"] for c in cells}):
        path = build / "bin" / binary
        add(f"binary {binary}", path.exists(), str(path))
    for allocator in sorted({c["allocator"] for c in cells}):
        entry = manifest.get(allocator)
        if entry is None:
            add(f"allocator {allocator}", False, "not in the manifest")
            continue
        library = entry.get("library")
        add(f"allocator {allocator}", not library or Path(library).exists(),
            library or "system allocator")
    return checks


def expand_cpu_list(text):
    """'0-2,5' -> ['0', '1', '2', '5']."""
    cpus = []
    for part in filter(None, text.split(",")):
        if "-" in part:
            lo, hi = part.split("-")
            cpus += [str(c) for c in range(int(lo), int(hi) + 1)]
        else:
            cpus.append(part)
    return cpus


def completed_runs(records_path):
    done = set()
    if records_path.exists():
        for line in records_path.read_text().splitlines():
            if line.strip():
                run = json.loads(line)["run"]
                done.add((run["cell"], run["repetition"]))
    return done


def command_for(cell, matrix, build, cpu, counters):
    cmd = []
    if cpu is not None:
        cmd += ["taskset", "-c", str(cpu)]
    cmd += [str(build / "bin" / cell["binary"]), *cell["args"],
            f"--counters={counters}"]
    return cmd


def check_record(record, cell):
    """Problems that mean the campaign itself is broken, so it should stop."""
    semantic = record["build"]["contractSemantic"]
    if semantic not in ("ignore", "none"):
        return (f"{cell['binary']} was built with contract semantic "
                f"'{semantic}'; timing runs need 'ignore' "
                "(configure with -DTIERALLOC_CONTRACT_SEMANTIC=ignore)")
    if record["allocator"]["name"] != cell["allocator"]:
        return (f"{cell['id']}: malloc is served by "
                f"{record['allocator']['name']}, not {cell['allocator']}; is "
                "the preload loading?")
    return None


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--matrix", type=Path, default=ROOT / "bench/matrix.json")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--deps", type=Path, default=ROOT / "build-deps")
    parser.add_argument("--runs", type=int)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--only", help="comma-separated workload names")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true",
                        help="run even if preflight errors")
    args = parser.parse_args()

    try:
        matrix = json.loads(args.matrix.read_text())
        manifest_path = args.deps / "manifest.json"
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())["allocators"]
        else:
            # Without scripts/build-allocators.sh (CI, a fresh checkout) only
            # the system allocator is known.
            print(f"bench_run: no {manifest_path}; only glibc is available",
                  file=sys.stderr)
            manifest = {"glibc": {"library": None, "runEnv": {},
                                  "hugepageEnv": {}}}
        only = set(args.only.split(",")) if args.only else None
        cells = expand_cells(matrix, only)
    except (OSError, json.JSONDecodeError, KeyError, MatrixError) as e:
        print(f"bench_run: {e}", file=sys.stderr)
        return 2
    if not cells:
        print("bench_run: no cells to run", file=sys.stderr)
        return 2
    for cell in cells:
        if cell["allocator"] not in manifest:
            print(f"bench_run: {cell['id']}: allocator {cell['allocator']} is "
                  f"not in {manifest_path}", file=sys.stderr)
            return 2

    runs = args.runs if args.runs is not None else matrix["runs"]
    cpu = args.cpu if args.cpu is not None else matrix.get("cpu")
    counters = matrix.get("counters", "required")
    hugepages = bool(matrix.get("hugepages", False))
    timeout = matrix.get("timeoutSeconds", 300)
    build = args.build.resolve()

    jobs = [(cell, rep) for rep, cell in itertools.product(range(runs), cells)]
    random.Random(matrix.get("seed", 1)).shuffle(jobs)

    if args.dry_run:
        for cell, rep in jobs:
            env = environment(cell["allocator"], manifest, hugepages)
            settings = " ".join(f"{k}={shlex.quote(env[k])}" for k in sorted(env)
                                if k.startswith(ALLOCATOR_ENV_PREFIXES))
            cmd = " ".join(shlex.quote(c) for c in
                           command_for(cell, matrix, build, cpu, counters))
            print(f"# {cell['id']} #{rep}\n{settings} {cmd}".replace("  ", " "))
        print(f"# {len(jobs)} runs: {len(cells)} cells x {runs}")
        return 0

    checks = preflight(matrix, cells, manifest, build, cpu)
    for c in checks:
        if not c["ok"]:
            print(f"{c['level'].upper():7} {c['check']}: {c['detail']}",
                  file=sys.stderr)
    blocked = [c for c in checks if c["level"] == "error" and not c["ok"]]
    if blocked and not args.force:
        print(f"bench_run: {len(blocked)} preflight error(s); fix them or "
              "pass --force (the campaign records that it was forced)",
              file=sys.stderr)
        return 1

    stamp = datetime.datetime.now(datetime.timezone.utc)
    out = args.out or ROOT / "results" / stamp.strftime("%Y%m%d-%H%M%S")
    (out / "logs").mkdir(parents=True, exist_ok=True)
    records_path = out / "records.jsonl"
    campaign_path = out / "campaign.json"
    matrix_text = args.matrix.read_text()
    if campaign_path.exists():
        campaign = json.loads(campaign_path.read_text())
        if campaign["matrixSha256"] != hashlib.sha256(matrix_text.encode()).hexdigest():
            print(f"bench_run: {out} was started with a different matrix; use "
                  "a new --out", file=sys.stderr)
            return 2
        # Resuming with different code mixes two versions in one campaign;
        # record it, and say so, rather than refuse (the fix may be the reason
        # for resuming).
        resumed = git_state(out / "logs")
        same = (resumed.get("commit") == campaign["git"].get("commit") and
                resumed.get("diffSha256") == campaign["git"].get("diffSha256"))
        if not same:
            print("bench_run: warning: the code changed since this campaign "
                  "started; the new state is recorded under resumes",
                  file=sys.stderr)
        campaign.setdefault("resumes", []).append(
            {"time": stamp.isoformat(), "sameCode": same,
             "git": resumed})
    else:
        campaign = {
            "id": out.name,
            "started": stamp.isoformat(),
            "matrix": matrix,
            "matrixSha256": hashlib.sha256(matrix_text.encode()).hexdigest(),
            "runs": runs, "cpu": cpu, "counters": counters,
            "hugepages": hugepages,
            "git": git_state(out),
            "machine": machine(cpu),
            "manifest": manifest,
            "cells": cells,
        }
    campaign["preflight"] = checks
    campaign["forced"] = campaign.get("forced", False) or (bool(blocked) and args.force)
    campaign["status"] = "running"
    campaign_path.write_text(json.dumps(campaign, indent=2) + "\n")

    done = completed_runs(records_path)
    todo = [(c, r) for c, r in jobs if (c["id"], r) not in done]
    print(f"bench_run: {out}: {len(todo)} of {len(jobs)} runs to do "
          f"({len(cells)} cells x {runs})", file=sys.stderr)

    status, failures, consecutive = "complete", 0, 0
    started = time.monotonic()
    with records_path.open("a") as records:
        for n, (cell, rep) in enumerate(todo, 1):
            cmd = command_for(cell, matrix, build, cpu, counters)
            env = environment(cell["allocator"], manifest, hugepages)
            t0 = time.monotonic()
            try:
                result = subprocess.run(cmd, env=env, capture_output=True,
                                        text=True, timeout=timeout)
                code, stdout, stderr = result.returncode, result.stdout, result.stderr
            except subprocess.TimeoutExpired as e:
                code, stdout, stderr = -1, "", f"timed out after {timeout} s\n{e.stderr or ''}"
            wall = time.monotonic() - t0
            lines = [l for l in stdout.splitlines() if l.strip()]
            if code == 0 and len(lines) != 1:
                code, stderr = -2, f"expected one record, got {len(lines)}\n{stderr}"
            if code != 0:
                failures += 1
                consecutive += 1
                log = out / "logs" / f"{cell['id'].replace('/', '_')}-{rep}.log"
                log.write_text(f"$ {shlex.join(cmd)}\nexit {code}\n{stderr}")
                print(f"FAILED  {cell['id']} #{rep} (exit {code}); see {log}",
                      file=sys.stderr)
                if code == 2:
                    print("bench_run: a binary rejected its arguments; the "
                          "matrix is wrong", file=sys.stderr)
                    status = "aborted"
                    break
                if consecutive >= MAX_CONSECUTIVE_FAILURES:
                    print(f"bench_run: {consecutive} failures in a row; "
                          "stopping", file=sys.stderr)
                    status = "aborted"
                    break
                continue
            consecutive = 0
            record = json.loads(lines[0])
            problem = check_record(record, cell)
            if problem:
                print(f"bench_run: {problem}", file=sys.stderr)
                status = "aborted"
                break
            record["run"] = {
                "campaign": campaign["id"], "cell": cell["id"],
                "workload": cell["workload"], "variant": cell["variant"],
                "variantGroup": cell["variantGroup"], "sweep": cell["sweep"],
                "configuration": cell["configuration"], "repetition": rep,
                "wallSeconds": wall,
            }
            records.write(json.dumps(record) + "\n")
            records.flush()
            elapsed = time.monotonic() - started
            eta = elapsed / n * (len(todo) - n)
            print(f"[{n:5}/{len(todo)}] {cell['id']} #{rep} {wall:5.1f}s "
                  f"(eta {eta / 60:4.1f} min)", file=sys.stderr, flush=True)

    campaign["status"] = status
    campaign["failures"] = campaign.get("failures", 0) + failures
    campaign["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    campaign_path.write_text(json.dumps(campaign, indent=2) + "\n")
    print(f"bench_run: {status}; {failures} failed run(s); {out}", file=sys.stderr)
    return 0 if status == "complete" else 1


if __name__ == "__main__":
    sys.exit(main())
