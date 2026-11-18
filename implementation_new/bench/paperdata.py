#!/usr/bin/env python3
"""Data files, tables and numbers for the paper from the benchmark results.

Usage: python3 bench/paperdata.py <output directory> <results directory> [<dijkstra.csv>]

The results directory is an output directory of bench/run_benchmarks.sh (results.csv); dijkstra.csv is the
output of bench/run_dijkstra.sh. Of several rows of the same configuration, the last one counts. Writes

  main-n<n>-B1024.dat  for the largest n and B = 1024, one row per queue: <env>_avg (average time per operation)
                       and <env>_worst (slowest operation) for every environment, in ms (scatter plot)
  full-rows.tex        macro \\fullRows: the same numbers as a table, average and slowest per environment
  nsweep.dat           for every n (B = 1024): per queue <q>_avg (average time per operation at RTT 50 ms),
                       <q>_kb (kilobytes sent and received per operation) and <q>_worst (slowest operation at
                       RTT 50 ms), in ms
  bsweep.dat           for every B (largest n): per queue <q>_avg and <q>_worst (ms, RTT 50 ms) and <q>_mem
                       (peak client records)
  comm-rows.tex        macro \\commRows: for the largest n and B = 1024, the peak client memory in records, the
                       average number of round trips per operation, the largest number of round trips of a
                       single operation and the kilobytes sent and received per operation; for Path Heap, the
                       client memory is its paths and stash plus the peak number of entries of its map (in
                       millions)
  dijkstra-rows.tex    macros \\dijkstraHeads (graph names and sizes, rules) and \\dijkstraRows: total time of
                       Dijkstra's algorithm per queue, with one group of four environments per graph
In all tables, a rule separates Path Heap, which is only oblivious, from the data-independent queues.
and prints a summary with checks (every run complete, no overflow, no wrong answer).
"""
import csv
import os
import sys
from collections import defaultdict

ORDER = ["levelwise", "streaming", "cached", "spread", "poh"]
# Row labels of the tables, with the citation keys of the paper for the baselines.
# The macros \\JLS, \\PRQstream, \\PRQbatch and \\PRQspread are defined in the paper.
NAMES = {"levelwise": "\\JLS~\\cite{SODA:JafLarSim21}", "streaming": "\\PRQstream", "cached": "\\PRQbatch",
         "spread": "\\PRQspread", "poh": "Path Heap~\\cite{SP:Shi20}"}
PROFILES = ["tee", "lan", "region", "wan"]
SWEEP_PROFILE = "wan"  # RTT 50 ms, the environment of the abstract
GRAPHS = {"USA-road-d.NY": "New York", "USA-road-d.FLA": "Florida"}


def load(path, key):
    last = {}
    for r in csv.DictReader(open(path)):
        last[key(r)] = r
    return list(last.values())


def tex_int(x):
    return "$" + f"{int(round(x)):,}".replace(",", "\\,") + "$"


def duration(ms):
    """A duration with two significant digits and a readable unit: ms, s, min, h (one decimal)."""
    def sig2(x):
        return f"{x:.2f}" if x < 1 else f"{x:.1f}" if x < 10 else f"{x:.0f}"
    if ms < 0.1:
        return f"${sig2(ms * 1000)}$~$\\mu$s"
    if ms < 1000:
        return f"${sig2(ms)}$~ms"
    s = ms / 1000
    if s < 60:
        return f"${sig2(s)}$~s"
    if s < 3600:
        return f"${sig2(s / 60)}$~min"
    return f"${s / 3600:.1f}$~h"


def main():
    out, d = sys.argv[1], sys.argv[2]
    dij = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    rows = load(os.path.join(d, "results.csv"), lambda r: (r["profile"], r["n"], r["B"], r["queue"], r["seed"]))
    ok = True
    for r in rows:
        if r["exit_code"] != "0" or r["overflows"] != "0" or r["mismatches"] != "0":
            print(f"PROBLEM: {r['queue']} n={r['n']} B={r['B']} {r['profile']}: exit {r['exit_code']}, "
                  f"overflows {r['overflows']}, mismatches {r['mismatches']}")
            ok = False
    by = defaultdict(list)  # (n, B or "", queue, profile) -> rows (seeds)
    for r in rows:
        if r["exit_code"] == "0":
            by[(int(r["n"]), r["B"], r["queue"], r["profile"])].append(r)

    def get(n, B, q, p):
        return by.get((n, "" if q == "poh" else str(B), q, p), [])

    def mean(n, B, q, p, f):
        rs = get(n, B, q, p)
        return sum(float(r[f]) for r in rs) / len(rs) if rs else None

    def kb(n, B, q):
        rs = get(n, B, q, "tee")
        if not rs:
            return None
        return sum((int(r["bytes_down"]) + int(r["bytes_up"])) / int(r["measured_ops"]) / 1000 for r in rs) / len(rs)

    ns = sorted({k[0] for k in by if k[1] in ("1024", "")})
    nmax = ns[-1]
    queues = [q for q in ORDER if any(k[2] == q for k in by)]

    def rule(q):
        return ["\\midrule"] if q == "poh" else []

    # Average and slowest operation, largest n: scatter data and the full table.
    with open(os.path.join(out, f"main-n{nmax}-B1024.dat"), "w") as f:
        f.write("queue " + " ".join(f"{p}_avg {p}_worst" for p in PROFILES) + "\n")
        for q in queues:
            f.write(q + " " + " ".join(f"{mean(nmax, 1024, q, p, 'avg_op_ms'):.6g} {mean(nmax, 1024, q, p, 'worst_op_ms'):.6g}"
                                       for p in PROFILES) + "\n")
    lines = []
    for q in queues:
        cells = [NAMES[q]] + [duration(mean(nmax, 1024, q, p, f)) for p in PROFILES for f in ("avg_op_ms", "worst_op_ms")]
        lines += rule(q) + [" & ".join(cells) + " \\\\"]
    open(os.path.join(out, "full-rows.tex"), "w").write("\\newcommand{\\fullRows}{%\n" + "\n".join(lines) + "}\n")

    # Sweep over n.
    with open(os.path.join(out, "nsweep.dat"), "w") as f:
        f.write("n logn " + " ".join(f"{q}_avg {q}_kb {q}_worst" for q in queues) + "\n")
        for n in ns:
            cells = [str(n), str(n.bit_length() - 1)]
            for q in queues:
                a, w, k = mean(n, 1024, q, SWEEP_PROFILE, "avg_op_ms"), mean(n, 1024, q, SWEEP_PROFILE, "worst_op_ms"), kb(n, 1024, q)
                cells += [f"{a:.6g}" if a else "nan", f"{k:.6g}" if k else "nan", f"{w:.6g}" if w else "nan"]
            f.write(" ".join(cells) + "\n")

    # Sweep over B, largest n.
    bs = sorted({int(k[1]) for k in by if k[0] == nmax and k[1]})
    bq = [q for q in queues if q != "poh"]
    with open(os.path.join(out, "bsweep.dat"), "w") as f:
        f.write("B " + " ".join(f"{q}_avg {q}_worst {q}_mem" for q in bq) + "\n")
        for B in bs:
            cells = [str(B)]
            for q in bq:
                rs = get(nmax, B, q, SWEEP_PROFILE)
                if rs:
                    cells += [f"{mean(nmax, B, q, SWEEP_PROFILE, 'avg_op_ms'):.6g}",
                              f"{mean(nmax, B, q, SWEEP_PROFILE, 'worst_op_ms'):.6g}",
                              str(max(int(r["peak_client_records"]) for r in rs))]
                else:
                    cells += ["nan"] * 3
            f.write(" ".join(cells) + "\n")

    # Communication and client memory per operation, largest n.
    lines = []
    for q in queues:
        rs = get(nmax, 1024, q, "tee")
        rt = sum(int(r["round_trips"]) / int(r["measured_ops"]) for r in rs) / len(rs)
        mx = max(int(r["max_round_trips_per_op"]) for r in rs)
        peak = max(int(r["peak_client_records"]) for r in rs)
        if q == "poh":
            entries = max(int(r["position_map_entries"]) for r in rs)
            mem = f"${peak} + {entries / 1e6:.1f}$\\,M"
        else:
            mem = tex_int(peak)
        lines += rule(q) + [" & ".join([NAMES[q], mem, f"${rt:.3f}$", tex_int(mx), f"${kb(nmax, 1024, q):.1f}$"]) + " \\\\"]
    open(os.path.join(out, "comm-rows.tex"), "w").write("\\newcommand{\\commRows}{%\n" + "\n".join(lines) + "}\n")

    # Dijkstra.
    if dij:
        drows = load(dij, lambda r: (r["graph"], r["queue"], r["profile"]))
        for r in drows:
            if r["exit_code"] != "0" or r["overflows"] != "0" or r["wrong_distances"] != "0":
                print(f"PROBLEM: Dijkstra {r['graph']} {r['queue']} {r['profile']}: exit {r['exit_code']}")
                ok = False
        dby = {(r["graph"], r["queue"], r["profile"]): r for r in drows}
        graphs = [g for g in GRAPHS if any(k[0] == g for k in dby)]
        info = {g: next(r for k, r in dby.items() if k[0] == g) for g in graphs}
        # Two header rows (graph name, its size), one column group of four environments per graph.
        heads = ["& " + " & ".join(f"\\multicolumn{{4}}{{c}}{{\\textbf{{{GRAPHS[g]}}}}}" for g in graphs) + " \\\\",
                 "& " + " & ".join(f"\\multicolumn{{4}}{{c}}{{\\scriptsize {tex_int(int(info[g]['vertices']))} vertices, "
                                   f"{tex_int(int(info[g]['queue_operations']))} operations}}" for g in graphs) + " \\\\"]
        rules = " ".join(f"\\cmidrule({'lr' if k + 1 < len(graphs) else 'l'}){{{2 + 4 * k}-{5 + 4 * k}}}" for k in range(len(graphs)))
        lines = []
        for q in queues:
            if all((g, q, "tee") in dby for g in graphs):
                lines += rule(q) + [NAMES[q] + " & " + " & ".join(duration(float(dby[(g, q, p)]["total_ms"])) for g in graphs for p in PROFILES) + " \\\\"]
        open(os.path.join(out, "dijkstra-rows.tex"), "w").write(
            "\\newcommand{\\dijkstraHeads}{%\n" + "\n".join(heads) + "\n" + rules + "}\n"
            "\\newcommand{\\dijkstraRows}{%\n" + "\n".join(lines) + "}\n")

    # Summary for the text.
    print(f"n values {ns}; B values {bs}")
    for n in (1 << 20, nmax):
        print(f"\nn = {n}, B = 1024")
        for p in PROFILES:
            ph = mean(n, 1024, "poh", p, "avg_op_ms")
            line = f"  {p:7}"
            for q in queues:
                a, w = mean(n, 1024, q, p, "avg_op_ms"), mean(n, 1024, q, p, "worst_op_ms")
                line += f" {q}: {a:.4g}/{w:.4g}" + (f" ({ph / a:.1f}x)" if q != "poh" else "")
            print(line)
        for q in queues:
            rs = get(n, 1024, q, "tee")
            r = rs[0]
            o = int(r["measured_ops"])
            print(f"  {q}: client {r['peak_client_records']}, map {r['position_map_entries'] or 0}, stash {r['peak_stash'] or 0}, "
                  f"enc us/op {float(r['encryption_ms']) * 1000 / o:.3f}, store us/op {float(r['store_ms']) * 1000 / o:.3f}, "
                  f"avg us/op {float(r['avg_op_ms']) * 1000:.3f}, objects/op {(int(r['objects_read']) + int(r['objects_written'])) / o:.3f}, "
                  f"measured {o}")
    print("\nALL RUNS CLEAN" if ok else "\nSOME RUNS HAVE PROBLEMS")


if __name__ == "__main__":
    main()
