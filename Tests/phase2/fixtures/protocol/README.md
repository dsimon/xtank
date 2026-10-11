# Protocol v1 golden fixtures

Committed golden wire bytes for the frozen protocol v1 schema
(`docs/PROTOCOL-v1.md`). They are the acceptance artifacts for P2.2 and the
byte targets the P2.3 codec must reproduce exactly.

| File | Contents |
| --- | --- |
| `hello.hex` | HELLO with the frozen content-set digest |
| `welcome.hex` | WELCOME |
| `reject_version.hex` | REJECT `XTK_ERR_BAD_VERSION` |
| `reject_content.hex` | REJECT `XTK_ERR_CONTENT_MISMATCH` |
| `catalog.hex` | CONTENT_CATALOG with the three allowlisted entries |
| `config_select.hex` | CONFIG_SELECT |
| `start.hex` | START |
| `epoch_begin.hex` | EPOCH_BEGIN |
| `results.hex` | RESULTS with two score entries |
| `event.hex` | HIT event |
| `cmd_ack.hex` | CMD_ACK (accepted + rejected) |
| `reliable_cmd.hex` | RELIABLE_CMD (fire edge) |
| `protocol_error.hex` | PROTOCOL_ERROR `XTK_ERR_BAD_CHANNEL` |
| `input_batch.hex` | INPUT_BATCH with drive + fire commands |
| `view_snapshot.hex` | keyframe VIEW_SNAPSHOT (self, 1 vehicle, 1 bullet, 1 map cell) |
| `endian.hex` | raw cross-endian u16/u32/i32/f64 (not enveloped) |
| `release-preset.txt` | canonical release-preset bytes hashed into the manifest |

## Format

`*.hex` holds lowercase hex octets, whitespace and `#` comments ignored. The
fixtures are authoritative; `gen_fixtures.py` only regenerates them for review.

## Verification

`verify.py` is an **independent** decoder: it reads each `*.hex`, decodes
fields at the offsets in `docs/PROTOCOL-v1.md`, and asserts them against the
hand-authored `expected-fields.txt`. It does not import the generator, so a
shared encoder/decoder bug cannot make a fixture self-consistent. Run:

```sh
python3 Tests/phase2/fixtures/protocol/verify.py
```

The full P2.2 acceptance driver is `Tests/phase2/protocol.sh`. Generated
binaries and logs are written under `${XTANK_P2_EVIDENCE:-/tmp/opencode/
xtank-phase2/p2.2}/protocol`, never into Git.
