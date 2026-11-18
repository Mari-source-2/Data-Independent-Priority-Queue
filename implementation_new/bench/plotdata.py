#!/usr/bin/env python3
"""Converts benchmark results into data files for the pgfplots figures of the paper.

Usage: python3 bench/plotdata.py [--all] <output directory for .dat files> <results directory>...

Each results directory is an output directory of bench/run_benchmarks.sh (results.csv, lat/), or any directory
with a results.csv. Of several rows for the same configuration, the last one counts; failed runs are skipped;
values are averaged over seeds (the latency tails use the first seed). Writes the files that the paper's
figures read:
  avgT-n<n>-B<B>.dat                  average time per operation (ms): one row per queue, one column per
                                      environment
  maxT-n<n>-B<B>.dat                  the same for the slowest single operation
and, with --all, also
  avg-n<n>-B<B>.dat, max-n<n>-B<B>.dat
                                      the same values transposed: one row per environment
  ccdf-<profile>-n<n>-B<B>-<queue>.dat
                                      tail of the time per operation: columns ms and the fraction of
                                      operations that take at least that long (needs lat/)
"""
import csv
import os
import sys
from collections import defaultdict

QUEUES = ["streaming", "cached", "spread", "poh"]
PROFILES = ["tee", "lan", "region", "wan"]


def load(dirs):
    last = {}
    for d in dirs:
        for r in csv.DictReader(open(os.path.join(d, "results.csv"))):
            r["_dir"] = d
            key = (r["profile"], r["n"], r["B"], r["queue"], r["seed"])
            if key in last and last[key].get("a", "") != r.get("a", ""):
                sys.exit(f"results with different relocation intervals a for {key}; plot them separately")
            last[key] = r
    rows = []
    for key, r in last.items():
        if r.get("exit_code", "0") not in ("0", "") or not r["avg_op_ms"]:
            print(f"skipping failed run {key}", file=sys.stderr)
            continue
        rows.append(r)
    return rows


def mean_over_seeds(rs, field):
    vals = [float(r[field]) for r in rs if r and r.get(field)]
    return sum(vals) / len(vals) if vals else None


def latency_file(r):
    if r["queue"] == "poh":
        name = f"poh-n{r['n']}-{r['profile']}-s{r['seed']}"
    else:
        a = f"-a{r['a']}" if r.get("a") else ""  # results written before the a column existed have none
        name = f"{r['queue']}-n{r['n']}-B{r['B']}{a}-{r['profile']}-s{r['seed']}"
    return os.path.join(r["_dir"], "lat", name + ".txt")


def write_ccdf(path, values_us, points=400):
    v = sorted(values_us)
    m = len(v)
    # Evenly spaced quantiles plus the whole upper tail, so that the rare slow operations are visible.
    idx = sorted(set([int(i * (m - 1) / (points - 1)) for i in range(points)] + list(range(max(0, m - 200), m))))
    with open(path, "w") as f:
        f.write("ms frac\n")
        for i in idx:
            f.write(f"{max(v[i], 0.1) / 1000:.6g} {(m - i) / m:.6g}\n")


def main():
    args = sys.argv[1:]
    everything = "--all" in args
    args = [a for a in args if a != "--all"]
    if len(args) < 2:
        sys.exit(__doc__)
    out = args[0]
    os.makedirs(out, exist_ok=True)
    rows = load(args[1:])
    poh = defaultdict(list)   # (n, profile) -> rows (one per seed)
    ours = defaultdict(lambda: defaultdict(list))  # (n, B) -> {(profile, queue): rows}
    for r in sorted(rows, key=lambda r: int(r["seed"] or 0)):
        if r["queue"] == "poh":
            poh[(r["n"], r["profile"])].append(r)
        else:
            ours[(r["n"], r["B"])][(r["profile"], r["queue"])].append(r)
    for (n, B), runs in ours.items():
        profiles = [p for p in PROFILES if any(k[0] == p for k in runs)]
        for kind, field in (("avg", "avg_op_ms"), ("max", "worst_op_ms")):
            if everything:
                with open(os.path.join(out, f"{kind}-n{n}-B{B}.dat"), "w") as f:
                    f.write("env " + " ".join(QUEUES) + "\n")
                    for prof in profiles:
                        vals = []
                        for q in QUEUES:
                            rs = poh.get((n, prof), []) if q == "poh" else runs.get((prof, q), [])
                            v = mean_over_seeds(rs, field)
                            vals.append(f"{v:.6g}" if v is not None else "nan")
                        f.write(f"{prof} " + " ".join(vals) + "\n")
            # One row per queue, for the horizontal bar charts of the paper.
            with open(os.path.join(out, f"{kind}T-n{n}-B{B}.dat"), "w") as f:
                f.write("queue " + " ".join(profiles) + "\n")
                for q in QUEUES:
                    vals = []
                    for prof in profiles:
                        rs = poh.get((n, prof), []) if q == "poh" else runs.get((prof, q), [])
                        v = mean_over_seeds(rs, field)
                        vals.append(f"{v:.6g}" if v is not None else "nan")
                    f.write(f"{q} " + " ".join(vals) + "\n")
        if not everything:
            continue
        for prof in profiles:
            for q in QUEUES:
                rs = poh.get((n, prof), []) if q == "poh" else runs.get((prof, q), [])
                r = rs[0] if rs else None
                if r and os.path.exists(latency_file(r)):
                    vals = [float(x) for x in open(latency_file(r)) if x.strip()]
                    write_ccdf(os.path.join(out, f"ccdf-{prof}-n{n}-B{B}-{q}.dat"), vals)


if __name__ == "__main__":
    main()
