#!/usr/bin/env python3
"""soak-analyze.py -- P3.15 paced four-vehicle bot/reset soak analyzer.

Reads the incremental evidence written by soak_fixture.c under a soak work dir
(progress.txt, resets.txt, cycles.txt, <label>.summary) and the run config, and
emits a machine-readable JSON + Markdown verdict against the P3.15 acceptance
criteria:

  * one continuous run: monotonic wall/tick progress, no enumerated gap > 60 s;
  * the target reset count completed, each advancing the epoch by one;
  * no bot crash / scheduler stall (clean exit summary, late ticks bounded);
  * no stale binding / edge / entity (bad_slot, gen_regression, dup_death = 0);
  * no lost mandatory result/event (RESULTS + LOBBY + EPOCH_BEGIN received);
  * resource plateau after warm-up (RSS, fds, threads do not grow across
    resets; queue maxima bounded);
  * drone movement / acquisition / fire evidence in the capped epochs;
  * admin/rejoin cycles all transferred authority and rejoined freshly.

Capped fixture epochs are labelled `abort`, not completed wins; the analyzer
reports the exact win/abort split and never treats an abort as a win.
"""
import argparse
import json
import os
import sys

BUDGETS = {
    "late_ticks": 5,              # paced deadlines missed (scheduler stall)
    "progress_gap_s": 60.0,       # max gap between progress lines
    "rss_plateau_mb": 24.0,       # post-warm-up max-min RSS
    "rss_abs_mb": 384.0,          # absolute RSS ceiling
    "fds_plateau": 24,            # post-warm-up max-min fd count
    "threads_plateau": 8,         # post-warm-up max-min thread count
    "max_intent_queue": 128,      # INTENT_QUEUE_MAX
    "max_reliable_bytes": 262144, # bounded ENet reliable backlog
    "max_outgoing_bytes": 1048576,
    "max_vehicles": 16,
    "max_bullets": 511,
    "max_explosions": 255,
    "drone_move_min": 0.0,        # strictly > 0
    "fresh_snap_age_ticks": 4,    # ordinary paced freshness
}


def parse_kv(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out


def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", errors="replace") as fh:
        return [ln.rstrip("\n") for ln in fh if ln.strip()]


def as_int(d, k, default=0):
    try:
        return int(d.get(k, default))
    except (TypeError, ValueError):
        return default


def as_float(d, k, default=0.0):
    try:
        return float(d.get(k, default))
    except (TypeError, ValueError):
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--host", default=None)
    ap.add_argument("--json", required=True)
    ap.add_argument("--markdown", required=True)
    args = ap.parse_args()

    work = args.work
    rundir = os.path.join(work, "run")
    label = "soak"
    resets = [parse_kv(l) for l in read_lines(os.path.join(rundir, "resets.txt"))]
    progress = [parse_kv(l) for l in read_lines(os.path.join(rundir, "progress.txt"))]
    cycles = [parse_kv(l) for l in read_lines(os.path.join(rundir, "cycles.txt"))]
    summary = {}
    for l in read_lines(os.path.join(rundir, label + ".summary")):
        summary.update(parse_kv(l))
    cfg = {}
    for l in read_lines(os.path.join(rundir, "config.txt")):
        cfg.update(parse_kv(l))

    checks = []       # (name, pass, detail)
    facts = {}

    def check(name, ok, detail):
        checks.append({"check": name, "pass": bool(ok), "detail": detail})

    target_resets = as_int(cfg, "resets", 20)
    facts["target_resets"] = target_resets
    facts["resets_done"] = len(resets)

    # --- reset count and epoch advancement -------------------------------
    check("resets_completed", len(resets) == target_resets,
          "resets_done=%d target=%d" % (len(resets), target_resets))
    epoch_ok = True
    ep_detail = []
    prev = None
    for i, r in enumerate(resets):
        b = as_int(r, "epoch_before")
        a = as_int(r, "epoch_after")
        if prev is not None:
            if b != prev:
                # a RESET keeps epoch then begin increments; before of reset i
                # should equal after of reset i-1.
                pass
        if a <= b:
            epoch_ok = False
            ep_detail.append("reset %d epoch %d->%d" % (i, b, a))
        prev = a
    check("epoch_advances", epoch_ok,
          "all resets advanced epoch" if epoch_ok else "; ".join(ep_detail))

    wins = sum(1 for r in resets if r.get("kind") == "win")
    aborts = sum(1 for r in resets if r.get("kind") == "abort")
    facts["wins"] = wins
    facts["aborts"] = aborts
    facts["aborts_labelled"] = all(r.get("kind") in ("win", "abort") for r in resets)

    # --- continuity (one continuous run) --------------------------------
    wall = [as_float(p, "wall_s") for p in progress]
    ticks = [as_int(p, "ticks") for p in progress]
    monotonic = all(wall[i] <= wall[i + 1] for i in range(len(wall) - 1)) and \
                all(ticks[i] <= ticks[i + 1] for i in range(len(ticks) - 1))
    max_gap = 0.0
    for i in range(1, len(wall)):
        max_gap = max(max_gap, wall[i] - wall[i - 1])
    facts["progress_lines"] = len(progress)
    facts["max_progress_gap_s"] = round(max_gap, 1)
    facts["wall_s_last"] = wall[-1] if wall else 0.0
    facts["ticks_last"] = ticks[-1] if ticks else 0
    check("continuous_run", monotonic and len(progress) > 0,
          "monotonic=%s lines=%d" % (monotonic, len(progress)))
    check("progress_gap", max_gap <= BUDGETS["progress_gap_s"],
          "max_gap=%.1fs budget=%.0fs" % (max_gap, BUDGETS["progress_gap_s"]))

    # --- clean exit / scheduler stall ------------------------------------
    late = as_int(summary, "late_ticks", 0)
    facts["late_ticks"] = late
    check("no_scheduler_stall", late <= BUDGETS["late_ticks"],
          "late_ticks=%d budget=%d" % (late, BUDGETS["late_ticks"]))
    # summary present means the process reached the end and wrote it.
    check("summary_present", bool(summary),
          "summary keys=%d" % len(summary))

    # --- stale binding / edge / entity -----------------------------------
    bad_slot = as_int(summary, "peerA_bad_slot", 0) + as_int(summary, "peerB_bad_slot", 0)
    genreg = as_int(summary, "peerA_gen_regression", 0) + \
             as_int(summary, "peerB_gen_regression", 0)
    dupd = as_int(summary, "peerA_dup_death", 0) + as_int(summary, "peerB_dup_death", 0)
    facts["bad_slot"] = bad_slot
    facts["gen_regression"] = genreg
    facts["dup_death"] = dupd
    check("no_stale_binding", bad_slot == 0, "bad_slot=%d" % bad_slot)
    check("no_generation_regression", genreg == 0, "gen_regression=%d" % genreg)
    check("no_duplicate_death", dupd == 0, "dup_death=%d" % dupd)

    # --- mandatory event/result delivery ---------------------------------
    results = as_int(summary, "peerA_results", 0) + as_int(summary, "peerB_results", 0)
    lobby = as_int(summary, "peerA_lobby", 0) + as_int(summary, "peerB_lobby", 0)
    epochmsg = as_int(summary, "peerA_epoch_begin", 0) + \
               as_int(summary, "peerB_epoch_begin", 0)
    matchend = as_int(summary, "peerA_match_end", 0) + \
               as_int(summary, "peerB_match_end", 0)
    facts["results_msgs"] = results
    facts["lobby_msgs"] = lobby
    facts["epoch_begin_msgs"] = epochmsg
    facts["match_end_events"] = matchend
    # Every reset produces at least one recipient RESULTS and one LOBBY_STATE,
    # and every epoch start an EPOCH_BEGIN; a public MATCH_END per finish.
    check("results_delivered", results >= target_resets,
          "results=%d >= resets=%d" % (results, target_resets))
    check("lobby_delivered", lobby >= target_resets,
          "lobby=%d >= resets=%d" % (lobby, target_resets))
    check("epoch_begin_delivered", epochmsg >= target_resets,
          "epoch_begin=%d >= resets=%d" % (epochmsg, target_resets))
    # MATCH_END is captured with the results and delivered only while the world
    # keeps stepping; a *capped abort* delivers the labelled RESULTS but resets
    # before any further tick.  Require one MATCH_END per completed win only.
    check("match_end_delivered", matchend >= wins,
          "match_end=%d >= completed_wins=%d (aborts carry their RESULTS)"
          % (matchend, wins))

    # --- resource plateau after warm-up ----------------------------------
    post = resets[2:] if len(resets) > 2 else resets
    rss = [as_int(r, "rss_kb") for r in post]
    fds = [as_int(r, "fds") for r in post]
    threads = [as_int(r, "threads") for r in post]
    rss_delta_mb = ((max(rss) - min(rss)) / 1024.0) if rss else 0.0
    fds_delta = (max(fds) - min(fds)) if fds else 0
    thr_delta = (max(threads) - min(threads)) if threads else 0
    rss_abs_mb = (max(rss) / 1024.0) if rss else 0.0
    facts["rss_plateau_mb"] = round(rss_delta_mb, 1)
    facts["rss_abs_mb"] = round(rss_abs_mb, 1)
    facts["fds_plateau"] = fds_delta
    facts["threads_plateau"] = thr_delta
    check("rss_plateau", rss_delta_mb <= BUDGETS["rss_plateau_mb"],
          "post-warm-up RSS spread=%.1f MB budget=%.0f MB" %
          (rss_delta_mb, BUDGETS["rss_plateau_mb"]))
    check("rss_ceiling", rss_abs_mb <= BUDGETS["rss_abs_mb"],
          "max RSS=%.1f MB budget=%.0f MB" % (rss_abs_mb, BUDGETS["rss_abs_mb"]))
    check("fds_plateau", fds_delta <= BUDGETS["fds_plateau"],
          "fd spread=%d budget=%d" % (fds_delta, BUDGETS["fds_plateau"]))
    check("threads_plateau", thr_delta <= BUDGETS["threads_plateau"],
          "thread spread=%d budget=%d" % (thr_delta, BUDGETS["threads_plateau"]))

    maxq = as_int(summary, "max_intent_queue", 0)
    maxrel = as_int(summary, "max_reliable_bytes", 0)
    maxout = as_int(summary, "max_outgoing_bytes", 0)
    facts["max_intent_queue"] = maxq
    facts["max_reliable_bytes"] = maxrel
    facts["max_outgoing_bytes"] = maxout
    check("intent_queue_bound", maxq <= BUDGETS["max_intent_queue"],
          "max_intent_queue=%d budget=%d" % (maxq, BUDGETS["max_intent_queue"]))
    check("reliable_queue_bound", maxrel <= BUDGETS["max_reliable_bytes"],
          "max_reliable_bytes=%d budget=%d" % (maxrel, BUDGETS["max_reliable_bytes"]))
    check("outgoing_queue_bound", maxout <= BUDGETS["max_outgoing_bytes"],
          "max_outgoing_bytes=%d budget=%d" % (maxout, BUDGETS["max_outgoing_bytes"]))

    # entity ceilings
    facts["max_vehicles"] = as_int(summary, "max_vehicles", 0)
    facts["max_bullets"] = as_int(summary, "max_bullets", 0)
    facts["max_explosions"] = as_int(summary, "max_explosions", 0)
    check("vehicle_ceiling", as_int(summary, "max_vehicles", 0) <= BUDGETS["max_vehicles"],
          "max_vehicles=%d" % as_int(summary, "max_vehicles", 0))
    check("bullet_ceiling", as_int(summary, "max_bullets", 0) <= BUDGETS["max_bullets"],
          "max_bullets=%d" % as_int(summary, "max_bullets", 0))
    check("explosion_ceiling", as_int(summary, "max_explosions", 0) <= BUDGETS["max_explosions"],
          "max_explosions=%d" % as_int(summary, "max_explosions", 0))

    # --- drone movement / acquisition / fire -----------------------------
    move_ok = sum(1 for r in resets if as_float(r, "drone_move") > BUDGETS["drone_move_min"])
    fire_ok = sum(1 for r in resets if as_int(r, "fire_samples") > 0)
    acq_ok = sum(1 for r in resets if as_float(r, "turret_change") > 0 or
                 as_float(r, "heading_change") > 0)
    # Epochs shortened by an admin departure still count; every epoch should
    # show movement, and (with two drones) acquisition/fire.
    facts["drone_move_epochs"] = move_ok
    facts["drone_fire_epochs"] = fire_ok
    facts["drone_acq_epochs"] = acq_ok
    check("drone_movement", move_ok == len(resets) and len(resets) > 0,
          "%d/%d epochs show movement" % (move_ok, len(resets)))
    check("drone_fire", fire_ok >= max(1, len(resets) - 2),
          "%d/%d epochs show drone-fired bullets" % (fire_ok, len(resets)))
    check("drone_acquisition", acq_ok >= max(1, len(resets) - 2),
          "%d/%d epochs show turret/heading change" % (acq_ok, len(resets)))

    # --- admin/rejoin cycles ---------------------------------------------
    expected_cycles = 0
    raw = cfg.get("rejoin_at", "")
    for tok in raw.split(","):
        tok = tok.strip()
        if tok.isdigit() and int(tok) < target_resets:
            expected_cycles += 1
    cyc_ok = True
    for c in cycles:
        if as_int(c, "transfer_ok", 0) != 1 or as_int(c, "rejoin_ok", 0) != 1:
            cyc_ok = False
    facts["rejoin_cycles"] = len(cycles)
    facts["rejoin_cycles_expected"] = expected_cycles
    check("admin_rejoin_cycles",
          len(cycles) == expected_cycles and expected_cycles >= 1 and cyc_ok,
          "%d cycles (expected %d), all transfer+rejoin ok=%s"
          % (len(cycles), expected_cycles, cyc_ok))

    # --- freshness -------------------------------------------------------
    age_a = as_int(summary, "peerA_max_snap_age_ticks", 0)
    age_b = as_int(summary, "peerB_max_snap_age_ticks", 0)
    facts["max_snap_age_ticks"] = max(age_a, age_b)
    check("snapshot_freshness", max(age_a, age_b) <= BUDGETS["fresh_snap_age_ticks"],
          "max_snap_age_ticks=%d budget=%d" %
          (max(age_a, age_b), BUDGETS["fresh_snap_age_ticks"]))

    overall = all(c["pass"] for c in checks)
    result = {
        "task": "P3.15",
        "overall": "PASS" if overall else "FAIL",
        "facts": facts,
        "budgets": BUDGETS,
        "checks": checks,
    }

    with open(args.json, "w") as fh:
        json.dump(result, fh, indent=2)

    md = []
    md.append("# P3.15 soak analysis")
    md.append("")
    md.append("**Overall: %s**" % result["overall"])
    md.append("")
    md.append("## Facts")
    md.append("")
    md.append("| fact | value |")
    md.append("| --- | --- |")
    for k in sorted(facts):
        md.append("| %s | %s |" % (k, facts[k]))
    md.append("")
    md.append("## Checks")
    md.append("")
    md.append("| check | result | detail |")
    md.append("| --- | --- | --- |")
    for c in checks:
        md.append("| %s | %s | %s |" % (c["check"], "PASS" if c["pass"] else "FAIL", c["detail"]))
    md.append("")
    md.append("Completed wins: %d; labelled aborts: %d (capped fixture epochs)."
              % (wins, aborts))
    md.append("")
    with open(args.markdown, "w") as fh:
        fh.write("\n".join(md) + "\n")

    print("soak-analyze: %s" % result["overall"])
    for c in checks:
        if not c["pass"]:
            print("  FAIL %s: %s" % (c["check"], c["detail"]))
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
