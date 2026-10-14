#!/usr/bin/env python3
"""impairment-analyze.py -- P3.14 measured impairment + resilience report.

Consumes the artifacts produced by Tests/phase3/impairment.sh:

  <work>/<label>/<label>.summary      fixture metrics (latency, integrity)
  <work>/<label>/relay-<label>.stats  relay configured/measured impairment
  <work>/<label>/<label>.authz.0/1    decoded per-recipient message streams
  <work>/<label>/relay-<label>.trace  per-datagram packet trace

For every profile it reports configured vs *measured* impairment (loss,
reorder, extra-delay rates and measured RTT), the latency and entity-integrity
budgets, and the recipient-authorization audit of the decoded per-recipient
streams.  Budget failures are collected and cause a non-zero exit so the
driver cannot mark a failing matrix as PASS.

The measured rates come from the relay's per-datagram accounting over observed
*real UDP* datagrams; the authorization audit decodes what each shipped client
actually received, so a capture can only contain information the recipient is
authorized to see.
"""
import argparse
import glob
import json
import os
import re
import sys


def read_kv(path):
    kv = {}
    if not os.path.exists(path):
        return kv
    with open(path) as f:
        for line in f:
            for tok in line.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    kv[k] = v
    return kv


def parse_relay_stats(path):
    cfg = {}
    clients = {}
    if not os.path.exists(path):
        return cfg, clients
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("client"):
                m = re.match(r"client(\d+)\s+(.*)", line)
                if m:
                    clients[int(m.group(1))] = read_tokens(m.group(2))
            else:
                for tok in line.split():
                    if "=" in tok:
                        k, _, v = tok.partition("=")
                        cfg[k] = v
    return cfg, clients


def read_tokens(s):
    d = {}
    for tok in s.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            d[k] = v
    return d


def fnum(d, k, default=0.0):
    try:
        return float(d.get(k, default))
    except (TypeError, ValueError):
        return default


def inum(d, k, default=0):
    try:
        return int(float(d.get(k, default)))
    except (TypeError, ValueError):
        return default


def audit_authz(path, bound_slot):
    """Return (lines, view_lines, bad_slots, slots_seen, ok)."""
    if not os.path.exists(path):
        return (0, 0, 0, set(), False)
    lines = views = bad = 0
    slots = set()
    with open(path, errors="replace") as f:
        for line in f:
            lines += 1
            if line.startswith("VIEW"):
                views += 1
                m = re.search(r"slot=(\d+)", line)
                if m:
                    s = int(m.group(1))
                    slots.add(s)
                    if s != bound_slot:
                        bad += 1
    return (lines, views, bad, slots, True)


def analyze_profile(label, summary_path, workdir):
    kv = read_kv(summary_path)
    stats_path = os.path.join(workdir, "relay-%s.stats" % label)
    cfg, clients = parse_relay_stats(stats_path)

    # configured (relay) impairment
    cfg_delay = fnum(cfg, "delay_ms")
    cfg_loss = fnum(cfg, "loss")
    cfg_reorder = fnum(cfg, "reorder")
    cfg_extra_prob = fnum(cfg, "extra_prob")
    cfg_extra_ms = fnum(cfg, "extra_delay_ms")

    sched = deliv = drop = reorder = extra = 0
    dsum = dmax = 0.0
    dcount = 0
    for i, c in sorted(clients.items()):
        sched += inum(c, "scheduled")
        deliv += inum(c, "delivered")
        drop += inum(c, "dropped")
        reorder += inum(c, "reordered")
        extra += inum(c, "extra")
        dsum += fnum(c, "delay_avg_ms")
        dcount += 1

    meas_loss = (drop / sched) if sched else 0.0
    meas_reorder = (reorder / sched) if sched else 0.0
    meas_extra = (extra / sched) if sched else 0.0
    meas_oneway = (dsum / dcount) if dcount else 0.0
    meas_rtt = 2.0 * meas_oneway

    rec = {
        "label": label,
        "summary": summary_path,
        "config": {
            "delay_ms_each_way": cfg_delay,
            "rtt_ms": 2 * cfg_delay,
            "loss": cfg_loss,
            "reorder": cfg_reorder,
            "extra_delay_ms": cfg_extra_ms,
            "extra_prob": cfg_extra_prob,
        },
        "measured": {
            "rtt_ms": meas_rtt,
            "oneway_ms": meas_oneway,
            "loss": meas_loss,
            "reorder": meas_reorder,
            "extra": meas_extra,
            "scheduled": sched,
            "delivered": deliv,
            "dropped": drop,
        },
        "ticks": inum(kv, "ticks"),
        "elapsed_ms": fnum(kv, "elapsed_ms"),
        "late_ticks": inum(kv, "late_ticks"),
        "lat_p95_ms_A": fnum(kv, "lat_p95_ms_A"),
        "lat_p95_ms_B": fnum(kv, "lat_p95_ms_B"),
        "p95_ms": max(fnum(kv, "lat_p95_ms_A"), fnum(kv, "lat_p95_ms_B")),
        "dup_death": inum(kv, "peer0_dup_death") + inum(kv, "peer1_dup_death"),
        "gen_regression": inum(kv, "peer0_gen_regression") +
        inum(kv, "peer1_gen_regression"),
        "max_snap_age_A": inum(kv, "peer0_max_snap_age_ticks"),
        "max_snap_age_B": inum(kv, "peer1_max_snap_age_ticks"),
        "freeze": {
            "on": inum(kv, "freeze_on"),
            "client": inum(kv, "freeze_client"),
            "at": inum(kv, "freeze_at"),
            "dur": inum(kv, "freeze_dur"),
            "removed_tick": inum(kv, "freeze_removed_tick", -1),
            "healthy_admin_index": inum(kv, "healthy_admin_index", -1),
            "healthy_self_index": inum(kv, "healthy_self_index", -1),
            "frozen_state_at_end": inum(kv, "frozen_state_at_end", -1),
        },
        "peers": {},
        "failures": [],
    }

    # per-peer metrics + authz audit
    for i in (0, 1):
        bs = inum(kv, "peer%d_bound_slot" % i, -1)
        bad_slot = inum(kv, "peer%d_bad_slot" % i)
        authz_path = os.path.join(workdir, "%s.authz.%d" % (label, i))
        alines, aviews, abad, aslots, aok = audit_authz(authz_path, bs)
        rec["peers"][str(i)] = {
            "bound_slot": bs,
            "view_msgs": inum(kv, "peer%d_view_msgs" % i),
            "bad_slot": bad_slot,
            "events": inum(kv, "peer%d_events" % i),
            "dup_death": inum(kv, "peer%d_dup_death" % i),
            "gen_regression": inum(kv, "peer%d_gen_regression" % i),
            "max_snap_age_ticks": inum(kv, "peer%d_max_snap_age_ticks" % i),
            "authz_lines": alines,
            "authz_view_lines": aviews,
            "authz_bad_slot": abad,
            "authz_slots": sorted(aslots),
            "authz_present": aok,
        }
        if aok and abad:
            rec["failures"].append(
                "peer%d authz: %d view messages with a foreign own_slot" %
                (i, abad))
        if bs >= 0 and bad_slot:
            rec["failures"].append(
                "peer%d received %d snapshot(s) for another slot" %
                (i, bad_slot))

    # budgets (P3.14)
    if label not in ("stress",) and rec["ticks"] > 0:
        if rec["p95_ms"] > 250.0:
            rec["failures"].append(
                "%s: p95 input-to-visible %.1f ms > 250 ms" %
                (label, rec["p95_ms"]))
    if rec["dup_death"]:
        rec["failures"].append("%s: %d duplicate death events" % (label, rec["dup_death"]))
    if rec["gen_regression"]:
        rec["failures"].append("%s: %d slot generation regressions" % (label, rec["gen_regression"]))
    # Snapshot age is client-view staleness.  The backpressure stage is a
    # deliberate extreme-loss/reorder/saturation stress (not a release
    # profile), so its unavoidable unreliable-snapshot gaps are reported but
    # not held to the <=1 s budget; the impairment matrix and the mandatory
    # combined run must still meet it.  Integrity checks (below) always apply.
    if label != "backpressure":
        for i in (0, 1):
            if rec["peers"][str(i)]["max_snap_age_ticks"] > 15:
                rec["failures"].append(
                    "%s: peer%d snapshot age %d ticks > 1 s" %
                    (label, i, rec["peers"][str(i)]["max_snap_age_ticks"]))

    # mandatory combined acceptance
    if label == "combined30":
        if abs(meas_rtt - 50.0) > 10.0:
            rec["failures"].append(
                "combined: measured RTT %.1f ms not ~50 ms" % meas_rtt)
        if not (0.002 <= meas_loss <= 0.03):
            rec["failures"].append(
                "combined: measured loss %.3f%% not ~1%%" % (meas_loss * 100))
        if not (0.002 <= meas_extra <= 0.03):
            rec["failures"].append(
                "combined: measured extra rate %.3f%% not ~1%%" %
                (meas_extra * 100))
        if rec["ticks"] < 26000:
            rec["failures"].append(
                "combined: only %d ticks (expected ~27000 / 30 min)" %
                rec["ticks"])
        if rec["elapsed_ms"] < 1750000:
            rec["failures"].append(
                "combined: elapsed %.0f s < 30 min" % (rec["elapsed_ms"] / 1000.0))

    # freeze acceptance
    if rec["freeze"]["on"]:
        fz = rec["freeze"]
        got = fz["removed_tick"] - fz["at"]
        if fz["at"] > 0 and not (25 <= got <= 40):
            rec["failures"].append(
                "freeze: peer removed %d ticks after freeze (expected ~30 / 2 s)" %
                got)
        if fz["healthy_admin_index"] != fz["healthy_self_index"]:
            rec["failures"].append(
                "freeze: administrator did not transfer to the healthy peer")
        if fz["frozen_state_at_end"] == 4:
            rec["failures"].append(
                "freeze: frozen client still RUNNING (must not reclaim)")

    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--host", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--markdown", default=None)
    args = ap.parse_args()

    profiles = []
    for sp in sorted(glob.glob(os.path.join(args.work, "*", "*.summary"))):
        label = os.path.basename(sp)[:-len(".summary")]
        profiles.append(analyze_profile(label, sp, os.path.dirname(sp)))

    out = {"work": args.work, "profiles": profiles}
    if args.host and os.path.exists(args.host):
        out["host"] = read_kv(args.host)

    all_failures = []
    for p in profiles:
        all_failures.extend(p["failures"])

    lines = []
    lines.append("# P3.14 impairment matrix")
    lines.append("")
    if out.get("host"):
        h = out["host"]
        lines.append("Host: nproc=%s ram_kb=%s kernel=%s gcc=%s (%s)" % (
            h.get("nproc"), h.get("ram_total_kb"), h.get("kernel"),
            h.get("gcc"), h.get("date")))
        lines.append("")
    lines.append("| profile | cfg RTT ms | meas RTT ms | cfg loss | meas loss | "
                 "meas reorder | meas extra | ticks | p95 ms | dup-death | "
                 "gen-reg | bad-slot |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for p in profiles:
        c = p["config"]
        m = p["measured"]
        lines.append("| %s | %.1f | %.1f | %.3f | %.3f%% | %.3f%% | %.3f%% | "
                     "%d | %.1f | %d | %d | %d |" % (
                         p["label"], c["rtt_ms"], m["rtt_ms"], c["loss"],
                         m["loss"] * 100, m["reorder"] * 100, m["extra"] * 100,
                         p["ticks"], p["p95_ms"], p["dup_death"],
                         p["gen_regression"],
                         sum(p["peers"][k]["bad_slot"] for k in p["peers"])))
    lines.append("")
    lines.append("## Recipient-authorization audit (decoded per-recipient streams)")
    lines.append("")
    for p in profiles:
        for k in ("0", "1"):
            pr = p["peers"][k]
            lines.append(
                "- %s peer%s: bound_slot=%d views=%d bad_slot=%d "
                "authz_lines=%d authz_bad_slot=%d slots=%s" % (
                    p["label"], k, pr["bound_slot"], pr["view_msgs"],
                    pr["bad_slot"], pr["authz_lines"], pr["authz_bad_slot"],
                    pr["authz_slots"]))
    lines.append("")
    if all_failures:
        lines.append("## FAILURES")
        for f in all_failures:
            lines.append("- " + f)
    else:
        lines.append("## Budgets: all PASS")
    md = "\n".join(lines) + "\n"

    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(md)

    print(md)
    return 1 if all_failures else 0


if __name__ == "__main__":
    sys.exit(main())
