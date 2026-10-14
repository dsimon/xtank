#!/usr/bin/env bash
#
# backpressure.sh -- P3.14 queued-reliable / fragment-loss / send-queue /
# reset-across-channels probe.
#
# Thin, explicit entry point over impairment.sh's `backpressure` stage so the
# P3.14 file set matches docs/PHASE3-PLAN.md (`Tests/phase3/{impairment.sh,
# backpressure.sh, impairment_fixture.c, impairment-analyze.py}`).  The same
# real-UDP impairment relay and two-human/two-drone fixture are used; see
# IMPAIRMENT-README.md.
#
#   bash Tests/phase3/backpressure.sh [--evidence DIR]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$HERE/impairment.sh" --stage backpressure "$@"
