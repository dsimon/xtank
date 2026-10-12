# Phase 3 owner decisions

## D3-A — Wire version: 1.1 (approved 2026-10-12)
Exact-version 1.1, new authoritative spec/fixtures, simultaneous client/server upgrade, explicit 1.0 mismatch rejection. No dual-version support.

## D3-B — Ready/start/reset and drone selection (approved 2026-10-12)
Admin picks maze and 0–2 drones; both humans explicitly ready against current config revision; config change wipes all readiness; admin sends explicit start. Ready/unready is an explicit toggleable field. Reset preserves prior maze/drone selection; reset from live/results returns to shared lobby as labeled abort, then fresh ready/start increments epoch.

## D3-C — Identity/barrier fixes and event recipient table (approved 2026-10-12)
u32 full entity-life generations in snapshots/input/results, true bullet/explosion generations, full-width content IDs, explicit reliable initial-view barrier, occurrence-ticked recipient-authorized events with per-recipient sequencing. VIEW-v1 knowledge unchanged. Session status + snapshot disappearance suffices for departure (no distinct REMOVE event); map_revision stays recipient-local export metadata.

## D3-D — Lone survivor and departed records (approved 2026-10-12)
No invented forfeit winner, no mid-match bot adds. Match continues under unchanged legacy Combat end conditions; no scored winner at end → abort/no-winner result. Departed combatants keep earned score/name in final results with departure marker; stable records preserved until outstanding projectile references and match cleanup complete. No fake kill credit.

## D3-E — Fairness, identity, pre-match timeouts (approved 2026-10-12)
Rotating peer-first bounded admission with per-peer quotas within existing global 64-service-event/128-intent bounds; guaranteed reserve for both neutralizations; explicit saturation acks; unchanged consume ordering. Duplicate player names rejected with actionable lobby error; connection identity independent of names. 3-second handshake deadline; valid lobby heartbeats reclaim abandoned peer slots; only valid fresh traffic counts as alive (malformed/stale/replayed does not).

## D3-F — Snapshot fragmentation (approved 2026-10-12)
Keep self-contained 15 Hz full views within existing 64 KiB app bounds; verify pinned ENet unreliable fragmentation + atomic reassembly, using its supported unreliable-fragment flag where needed. No silent downgrade of oversized views to reliable backlog. If fragment loss fails the impairment matrix, a measured chunking/compression proposal comes back to owner — no redesign pre-approved.

## D3-G — Native hosts, operational runs, CI hand-off (approved 2026-10-12)
Two clean Windows 11 x64 PCs + recorded ≥4-core/8 GB Ubuntu host for two-PC acceptance. Phase 2 native/solo debt closed before large Phase 3 integration. Existing bars unchanged: 250 ms p95, 15 Hz/p99 tick, 30-minute impairment, 60-minute/twenty-reset soak. Operational/native runs authorized.
