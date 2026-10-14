# P3.14 — approved impairment and slow-client matrix

How to reproduce the P3.14 evidence.

## Why a userspace relay

Kernel `netem`/`tbf` is **not** available in the reference sandbox: there is no
`/lib/modules`, `modprobe sch_netem` fails and `tc qdisc add dev lo root netem
...` returns `Error: Specified qdisc kind is unknown` (verbatim in
`P3.14-CHECKPOINT.md` and `~/workspace/coding/xtank-evidence/p3.14/s0/`).
Missing real link impairment would be BLOCKED, so P3.14 applies impairment with
`Tests/phase3/impairment_relay.c`, a userspace middlebox that forwards **real
loopback UDP datagrams** between the shipped ENet server and two shipped
clients. It is not an in-memory transport: the relay is a separate process, the
server and clients use their real ENet sockets, and every datagram is delayed,
dropped, reordered or duplicated at the UDP layer. `tests/phase3/impairment.sh`
measures the applied rates and RTT from the relay's per-datagram accounting.

## Build and run

```sh
export LC_ALL=C XTANK_SEED=114336
# optional: export XTANK_P3_EVIDENCE=/path/to/evidence
bash Tests/phase3/impairment.sh --stage build
bash Tests/phase3/impairment.sh --stage profiles   # baseline/delay/loss/reorder/combined
bash Tests/phase3/impairment.sh --stage freeze     # 10 s frozen client
bash Tests/phase3/impairment.sh --stage backpressure
bash Tests/phase3/impairment.sh --stage replay     # applied-log exact replay
bash Tests/phase3/impairment.sh --stage combined   # MANDATORY 30-minute run
bash Tests/phase3/impairment.sh --stage analyze

# Explicit, plan-listed backpressure entry point (same as the stage above):
bash Tests/phase3/backpressure.sh
```

The relays bind the client-facing socket to the port each client connects to
and a *distinct* server-facing ephemeral port; sharing one port would let
`SO_REUSEPORT` hashing deliver a client datagram to the connected
server-facing socket and stall bring-up. The fixture waits for the server's
reliable all-ready lobby broadcast before requesting start (README readiness),
so it never floods the fixed-tick per-peer admission quota.

Stages are resumable; `all` runs everything except the 30-minute `combined`
stage. Evidence is written under
`${XTANK_P3_EVIDENCE:-$HOME/workspace/coding/xtank-evidence/p3.14}/work/`:

- `<label>/<label>.summary` — fixture metrics (latency, integrity, queue maxima)
- `<label>/relay-<label>.stats` — configured vs measured impairment
- `<label>/relay-<label>.trace` — per-datagram packet trace
- `<label>/<label>.authz.{0,1}` — decoded per-recipient message streams
- `<label>/cap_client{0,1}_s2c.bin` — framed delivered server→client payloads
- `report.md` / `report.json` — analyzer output (budget FAILs fail the run)

## Impairment profiles

| profile | configured |
|---|---|
| baseline | none |
| delay-only | symmetric 25 ms each way (~50 ms RTT) |
| loss-only | 1% loss |
| reorder-only | 5% reorder (+20 ms) |
| combined | symmetric 25 ms (~50 ms RTT), 1% loss, 1% +100 ms |
| combined30 | the mandatory 30-minute four-vehicle combined run |

## Authorization audit

Every shipped client decodes its own inbound stream; the fixture records each
delivered message to `<label>.authz.N`. `impairment-analyze.py` checks that
every `VIEW` snapshot's `own_slot` equals the recipient's server-bound slot and
that no recipient ever receives a view for another slot. The relay's framed
`cap_client*_s2c.bin` captures are retained as the raw packet trace.
