#!/usr/bin/env python3
"""Summarizes bench/run_benchmarks.sh results: one table per n, buffer size B and environment.

Usage: python3 bench/summarize.py results.csv

Times are per operation, in milliseconds. Client memory is the peak number of records held by the
client; for Path Heap, its map from keys to references is listed separately.
"""
import csv
import sys
from collections import defaultdict

ORDER = ["streaming", "cached", "spread", "poh"]
NAMES = {"streaming": "Ours, buckets", "cached": "Ours, path", "spread": "Ours, spread", "poh": "Path Heap"}
PROFILE_ORDER = ["tee", "lan", "region", "wan"]


def load(path):
    """Rows of results.csv; the last row of every configuration wins, and incomplete runs are skipped."""
    last = {}
    for r in csv.DictReader(open(path)):
        key = (r["mode"], r["profile"], r["n"], r["B"], r["queue"], r["seed"])
        if key in last and last[key].get("a", "") != r.get("a", ""):
            sys.exit(f"results with different relocation intervals a for {key}; summarize them separately")
        last[key] = r
    rows = []
    for key, r in last.items():
        if r.get("exit_code", "0") not in ("0", "") or not r["avg_op_ms"]:
            print(f"skipping failed run {key} (exit code {r.get('exit_code')})", file=sys.stderr)
            continue
        rows.append(r)
    return rows


def main():
    rows = load(sys.argv[1])
    poh = {(r["n"], r["profile"], r["seed"]): r for r in rows if r["queue"] == "poh"}
    groups = defaultdict(list)
    for r in rows:
        if r["queue"] != "poh":
            groups[(r["n"], r["B"], r["profile"], r["seed"])].append(r)
    for (n, B, prof, seed), rs in sorted(groups.items(), key=lambda kv: (int(kv[0][0]), int(kv[0][1]), PROFILE_ORDER.index(kv[0][2]))):
        p = poh.get((n, prof, seed))
        first = rs[0]
        net = "no network" if first["rtt_ms"] == "0" else f"RTT {first['rtt_ms']} ms, {first['bandwidth_mbit']} Mbit/s"
        print(f"\nn = {n}, B = {B}, {prof} ({net}), seed {seed}")
        print(f"{'queue':22} {'client rec.':>11} {'map':>8} {'avg':>9} {'p50':>9} {'p99':>9} {'p99.9':>9} "
              f"{'max':>9} {'RT/op':>6} {'RTmax':>5} {'PH/ours':>8}")
        base = float(p["avg_op_ms"]) if p else None
        for r in sorted(rs + ([p] if p else []), key=lambda r: ORDER.index(r["queue"])):
            avg = float(r["avg_op_ms"])
            rt = int(r["round_trips"]) / max(1, int(r["measured_ops"]))
            ratio = f"{base / avg:7.1f}x" if base and r["queue"] != "poh" else ""
            flag = "" if r["mismatches"] == "0" and r["overflows"] == "0" else "  WRONG ANSWERS/OVERFLOW"
            label = NAMES[r["queue"]] + (f" a={r['a']}" if r.get("a") else "")
            print(f"{label:22} {int(r['peak_client_records']):>11} {r['position_map_entries'] or 0:>8} "
                  f"{avg:9.4f} {float(r['p50_op_ms']):9.4f} {float(r['p99_op_ms']):9.3f} {float(r['p999_op_ms']):9.3f} "
                  f"{float(r['worst_op_ms']):9.2f} {rt:6.3f} {r['max_round_trips_per_op']:>5} {ratio:>8}{flag}")


if __name__ == "__main__":
    main()
