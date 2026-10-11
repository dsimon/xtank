# Xtank Windows — Phase 2 owner decisions (2026-10-11)

Decisions D2-A–D2-F from `docs/PHASE2-PLAN.md` §3, ruled by the owner on 2026-10-11. These are recommendations accepted as rulings, not amendments to the eight Phase 0 approvals in `docs/DECISIONS.md`.

## D2-A: Rendering/input — APPROVED: SDL3

C frontend, one window with logical panels, full-frame redraw. Pin a tested SDL3 release and runtime distribution.

## D2-B: Transport — APPROVED: ENet over UDP

Direct IP/port, pinned maintained upstream release. Owner confirmed internet play is the ultimate goal; ENet's unreliable-sequenced snapshots + reliable-ordered control/events is the right pattern (TCP head-of-line blocking would stutter on lossy links).

## D2-C: Wire protocol and content identity — APPROVED: explicit binary protocol v1

Fixed-width fields, network byte order, explicit bounds, SHA-256 content manifest, exact version rejection. Schema frozen in P2.2 with owner protocol approval.

## D2-D: Windows build toolchain — APPROVED: MinGW-w64 cross-build from Linux

CMake for new client/protocol/transport code; server keeps GNUmakefile. MSVC remains a possible later toolchain, not a Phase 2 commitment. If cross-built binaries fail on clean Windows, work stops and returns to owner before switching.

## D2-E: Presentation timing — APPROVED: 15 Hz snapshots, one-tick interpolation

Full authorized snapshots at 15 Hz, one-tick (66.7 ms) interpolation buffer, 60 FPS render target (30/144 FPS checks). No extrapolation or local prediction. Healthy-LAN input-to-visible-action budget ≤250 ms; a two-tick buffer would be a measured tradeoff requiring owner review.

## D2-F: Session/view minimum — APPROVED (provisional): one-peer lobby, 2-second solo abort

First peer administers start/reset; server-enforced ownership and knowledge from first snapshot. Two seconds without valid traffic → neutral drive/fire, session abort, cleanup, return to lobby. Owner marked "for now" — revisit if the abort policy needs refinement. Phase 3 retains the full approved remaining-human departure policy.

## Pinned implementation versions (P2.1)

The exact SDL3, ENet and MinGW-w64 build/dependency versions — with download URLs and SHA-256 hashes — frozen for Phase 2 are recorded in `P2.1-REPORT.md`. No dependency substitution is authorized without a new owner decision.
