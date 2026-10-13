# Protocol v1.1 golden fixtures

Committed golden wire bytes for the frozen protocol-v1.1 schema
(`docs/PROTOCOL-v1.1.md`), frozen by P3.3. They are the byte targets the
v1.1 codec in `Protocol/` must reproduce exactly.

The Phase 2 v1.0 fixtures in `Tests/phase2/fixtures/protocol/` are **retained
unchanged**; a v1.1 codec must reject a 1.0 envelope with no fallback (D3-A).
`v1.0-fixtures.sha256` records their byte identity and `Tests/phase3/protocol.sh`
re-verifies it.

| File | Contents |
| --- | --- |
| `hello.hex` | HELLO (proto 1.1) with the frozen content-set digest |
| `welcome.hex` | WELCOME with human capacity |
| `reject_version.hex` | REJECT `XTK_ERR_VERSION_MISMATCH` |
| `reject_content.hex` | REJECT `XTK_ERR_CONTENT_MISMATCH` |
| `reject_duplicate_name.hex` | REJECT `XTK_ERR_DUPLICATE_NAME` |
| `catalog.hex` | CONTENT_CATALOG with the three allowlisted entries |
| `config_select.hex` | CONFIG_SELECT with drone count |
| `config_status.hex` | CONFIG_STATUS with drone count + config revision |
| `ready.hex` | READY with explicit ready flag + config revision |
| `start.hex` | START with full-width vehicle id, drone count, revision |
| `start_request.hex` | START_REQUEST |
| `epoch_begin.hex` | EPOCH_BEGIN |
| `lobby_state.hex` | LOBBY_STATE with a two-human roster |
| `results.hex` | RESULTS: u32 generation + departure marker |
| `reset.hex` | RESET |
| `quit.hex` | QUIT (zero-length) |
| `event.hex` | HIT event with capture tick |
| `cmd_ack.hex` | CMD_ACK (accepted + stale-life rejection) |
| `reliable_cmd.hex` | RELIABLE_CMD (fire edge, life generation) |
| `protocol_error.hex` | PROTOCOL_ERROR `XTK_ERR_BAD_CHANNEL` |
| `input_batch.hex` | INPUT_BATCH with drive + fire, life generation |
| `view_snapshot.hex` | keyframe VIEW_SNAPSHOT (u32 lifetimes) |
| `view_keyframe.hex` | reliable VIEW_KEYFRAME (initial-view barrier) |
| `endian.hex` | raw cross-endian u16/u32/i32/f64 (not enveloped) |

## Format

`*.hex` holds lowercase hex octets, whitespace and `#` comments ignored. The
fixtures are authoritative; `gen_fixtures.py` only regenerates them for review.

## Verification

`verify.py` is an **independent** decoder: it reads each `*.hex`, decodes
fields at the offsets in `docs/PROTOCOL-v1.1.md`, and asserts them against the
hand-authored `expected-fields.txt`. It does not import the generator and does
not call the C codec, so a shared encoder/decoder bug cannot make a fixture
self-consistent.

```sh
python3 Tests/phase3/fixtures/protocol/verify.py
```

The full P3.3 acceptance drivers are `Tests/phase3/protocol.sh` and
`Tests/phase3/codec.sh`. Generated binaries and logs are written under
`${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.3}/`, never into Git.
