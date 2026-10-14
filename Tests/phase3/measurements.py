#!/usr/bin/env python3
"""measurements.py -- P3.13 timing/bandwidth/latency analysis.

Reads the key=value summaries and TIMING component reports produced by
Tests/phase3/timing.sh (latency_fixture.c) and emits a consolidated JSON plus a
human-readable markdown table.  It also re-proves the P3.13 budgets so the
numbers in the report cannot silently drift:

  - exactly 900 ticks / 60 s (+/-1) at 30/144/mixed render rates,
  - total per-tick p99 < 66.7 ms and per-component p50/p95/p99/max recorded,
  - same-client input-to-visible p95 <= 250 ms,
  - bytes/s per peer and aggregate, wire fragment/header overhead.

Usage:
  measurements.py --dir DIR [--host host.txt] [--out report.json]
                  [--markdown report.md]
"""

import argparse
import json
import os
import re
import sys


def parse_kv(path):
    """Parse key=value files that may pack several pairs on one line."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or "=" not in line:
                continue
            for k, v in re.findall(r"([A-Za-z_]\w*)=([^\s=]+)", line):
                out[k] = v
    return out


def parse_timing(path):
    comps = {}
    if not os.path.exists(path):
        return comps
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("TIMING "):
                continue
            row = dict(re.findall(r"(\w+)=([\w.+-]+)", line))
            if "comp" in row:
                comps[row["comp"]] = {
                    "samples": int(row.get("samples", 0)),
                    "avg_us": float(row.get("avg_us", 0)),
                    "p50_us": float(row.get("p50_us", 0)),
                    "p95_us": float(row.get("p95_us", 0)),
                    "p99_us": float(row.get("p99_us", 0)),
                    "max_us": float(row.get("max_us", 0)),
                }
    return comps


def fnum(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except (TypeError, ValueError):
        return default


def compute_run(name, summary, timing):
    elapsed_s = fnum(summary, "elapsed_ms") / 1000.0
    agg = fnum(summary, "tx_bytes_agg")
    wire = fnum(summary, "wire_bytes")
    app_agg = agg
    run = {
        "name": name,
        "mode": summary.get("mode"),
        "maze": summary.get("maze"),
        "drones": summary.get("drones"),
        "fpsA": summary.get("fpsA"),
        "fpsB": summary.get("fpsB"),
        "ticks": int(fnum(summary, "ticks")),
        "target_ticks": summary.get("target_ticks"),
        "elapsed_s": round(elapsed_s, 3),
        "late_ticks": int(fnum(summary, "late_ticks")),
        "status": summary.get("status"),
        "components": timing,
        "total": {
            "samples": int(fnum(summary, "total_samples")),
            "p50_us": fnum(summary, "total_p50_us"),
            "p95_us": fnum(summary, "total_p95_us"),
            "p99_us": fnum(summary, "total_p99_us"),
            "max_us": fnum(summary, "total_max_us"),
        },
        "bytes": {
            "peer0": int(fnum(summary, "tx_bytes_peer0")),
            "peer1": int(fnum(summary, "tx_bytes_peer1")),
            "aggregate": int(agg),
            "wire": int(wire),
            "wire_packets": int(fnum(summary, "wire_packets")),
            "sendmsg_bytes": int(fnum(summary, "sendmsg_bytes")),
            "unreliable_fragments": int(fnum(summary, "unreliable_fragments")),
            "overhead_bytes": int(wire - app_agg) if wire >= app_agg else 0,
        },
        "bytes_per_s": {
            "peer0": round(fnum(summary, "tx_bytes_peer0") / elapsed_s, 1)
            if elapsed_s else 0.0,
            "peer1": round(fnum(summary, "tx_bytes_peer1") / elapsed_s, 1)
            if elapsed_s else 0.0,
            "aggregate": round(agg / elapsed_s, 1) if elapsed_s else 0.0,
        },
        "maxima": {
            "reliable_bytes": int(fnum(summary, "max_reliable_bytes")),
            "outgoing_bytes": int(fnum(summary, "max_outgoing_bytes")),
            "intent_queue": int(fnum(summary, "max_intent_queue")),
            "rx_queue": int(fnum(summary, "rx_queue_max")),
            "vehicles": int(fnum(summary, "max_vehicles")),
            "bullets": int(fnum(summary, "max_bullets")),
            "explosions": int(fnum(summary, "max_explosions")),
            "map_cells": int(fnum(summary, "max_map_cells")),
            "landmarks": int(fnum(summary, "max_landmarks")),
            "radar": int(fnum(summary, "max_radar")),
            "view_bytes": int(fnum(summary, "max_view_bytes")),
            "rss_hwm_kb": int(fnum(summary, "rss_hwm_kb")),
        },
        "latency": {
            "A": {
                "samples": int(fnum(summary, "lat_samples_A")),
                "p50_ms": fnum(summary, "lat_p50_ms_A"),
                "p95_ms": fnum(summary, "lat_p95_ms_A"),
                "p99_ms": fnum(summary, "lat_p99_ms_A"),
                "max_ms": fnum(summary, "lat_max_ms_A"),
            },
            "B": {
                "samples": int(fnum(summary, "lat_samples_B")),
                "p50_ms": fnum(summary, "lat_p50_ms_B"),
                "p95_ms": fnum(summary, "lat_p95_ms_B"),
                "p99_ms": fnum(summary, "lat_p99_ms_B"),
                "max_ms": fnum(summary, "lat_max_ms_B"),
            },
        },
    }
    if summary.get("mode") == "stress":
        run["bound"] = {
            "view_bytes": int(fnum(summary, "bound_view_bytes")),
            "encode_ok": int(fnum(summary, "bound_encode_ok")),
            "fragment_est": int(fnum(summary, "bound_fragment_est")),
            "vehicle_max": int(fnum(summary, "bound_vehicle_max")),
            "bullet_max": int(fnum(summary, "bound_bullet_max")),
            "explosion_max": int(fnum(summary, "bound_explosion_max")),
        }
    return run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--host")
    ap.add_argument("--out")
    ap.add_argument("--markdown")
    args = ap.parse_args()

    d = args.dir
    runs = []
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".summary"):
            continue
        name = fn[:-len(".summary")]
        summary = parse_kv(os.path.join(d, fn))
        timing = parse_timing(os.path.join(d, name + ".timing"))
        runs.append(compute_run(name, summary, timing))

    host = {}
    if args.host and os.path.exists(args.host):
        host = parse_kv(args.host)

    pace = [r for r in runs if r["mode"] == "pace"]
    stress = [r for r in runs if r["mode"] == "stress"]

    problems = []
    for r in pace:
        if not (899 <= r["ticks"] <= 901):
            problems.append(f"{r['name']}: ticks={r['ticks']} != 900 +/-1")
        if r["late_ticks"] != 0:
            problems.append(f"{r['name']}: late_ticks={r['late_ticks']}")
        if r["total"]["p99_us"] >= 66700:
            problems.append(f"{r['name']}: total p99 {r['total']['p99_us']}us")
        for who in ("A", "B"):
            if r["latency"][who]["p95_ms"] > 250:
                problems.append(
                    f"{r['name']}: lat p95 {who} "
                    f"{r['latency'][who]['p95_ms']}ms > 250")
    for r in stress:
        if r.get("bound", {}).get("encode_ok") != 2:
            problems.append(f"{r['name']}: max view bound did not round-trip")

    report = {
        "host": host,
        "host_meets_d3g": (
            int(host.get("nproc", 0)) >= 4
            and int(host.get("ram_total_kb", 0)) >= 8 * 1024 * 1024
        ),
        "budgets": {
            "ticks_per_60s": 900,
            "ticks_tolerance": 1,
            "total_p99_max_us": 66700,
            "latency_p95_max_ms": 250,
        },
        "runs": runs,
        "problems": problems,
        "pass": not problems,
    }

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)

    lines = ["# P3.13 measurements", ""]
    lines.append(f"- host: {host.get('nproc')} cores, "
                 f"{host.get('ram_total_kb')} kB RAM, kernel "
                 f"{host.get('kernel')}")
    lines.append(f"- meets D3-G reference (>=4 cores / 8 GB): "
                 f"**{report['host_meets_d3g']}**")
    lines.append("")
    if pace:
        lines.append("## Paced runs (900 ticks / 60 s)")
        lines.append("")
        lines.append("| run | fps A/B | ticks | elapsed s | late | "
                     "total p50/p95/p99/max us | lat p95 A/B ms | "
                     "agg bytes/s | wire overhead B |")
        lines.append("|---|---|---|---|---|---|---|---|---|")
        for r in pace:
            t = r["total"]
            lines.append(
                f"| {r['name']} | {r['fpsA']}/{r['fpsB']} | {r['ticks']} | "
                f"{r['elapsed_s']} | {r['late_ticks']} | "
                f"{t['p50_us']:.0f}/{t['p95_us']:.0f}/{t['p99_us']:.0f}/"
                f"{t['max_us']:.0f} | "
                f"{r['latency']['A']['p95_ms']:.1f}/"
                f"{r['latency']['B']['p95_ms']:.1f} | "
                f"{r['bytes_per_s']['aggregate']:.0f} | "
                f"{r['bytes']['overhead_bytes']} |")
        lines.append("")
        lines.append("### Per-component per-tick work (us)")
        lines.append("")
        lines.append("| run | component | p50 | p95 | p99 | max |")
        lines.append("|---|---|---|---|---|---|")
        for r in pace:
            for comp in ("service", "sim", "export", "encode"):
                c = r["components"].get(comp)
                if not c:
                    continue
                lines.append(f"| {r['name']} | {comp} | {c['p50_us']:.0f} | "
                             f"{c['p95_us']:.0f} | {c['p99_us']:.0f} | "
                             f"{c['max_us']:.0f} |")
        lines.append("")
    if stress:
        lines.append("## Stress maxima")
        lines.append("")
        lines.append("| run | vehicles | bullets | explosions | view B | "
                     "reliable B | outgoing B | intent q | RSS kB |")
        lines.append("|---|---|---|---|---|---|---|---|---|")
        for r in stress:
            m = r["maxima"]
            lines.append(f"| {r['name']} | {m['vehicles']} | {m['bullets']} | "
                         f"{m['explosions']} | {m['view_bytes']} | "
                         f"{m['reliable_bytes']} | {m['outgoing_bytes']} | "
                         f"{m['intent_queue']} | {m['rss_hwm_kb']} |")
        for r in stress:
            b = r.get("bound", {})
            lines.append("")
            lines.append(f"- max authorized view bound: {b.get('view_bytes')} B, "
                         f"round-trip ok={b.get('encode_ok')}, "
                         f"~{b.get('fragment_est')} fragments "
                         f"(engine maxima {b.get('vehicle_max')}/"
                         f"{b.get('bullet_max')}/{b.get('explosion_max')})")
        lines.append("")
    lines.append("## Verdict")
    lines.append("")
    if problems:
        for p in problems:
            lines.append(f"- FAIL: {p}")
    else:
        lines.append("- **PASS**: all P3.13 budgets satisfied.")

    md = "\n".join(lines) + "\n"
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as fh:
            fh.write(md)
    print(md)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
