# Voice notes: endpoint API reference and runbook

Offline voice notes from a gadget arrive over HTTP, get transcribed and
summarized on the host, and land as markdown in Obsidian. The normative
firmware⇄Hermes contract is
[the voice-note upload contract](../docs/voice-note-ingestion.md); the
firmware's own half (SD layout, `note.json`, the uploader loop) is
[the integration spec, §7–§8](../docs/waveshare-epaper-154-v2.md). This page
is the host-side API reference and runbook: how to call the endpoint, what it
answers, where the notes land, how failures retry, how to diagnose problems,
and how to run the tests that pin all of it.

The tests that pin it: `tests/test_notes_ingest.py` (the wire contract),
`tests/test_notes_pipeline.py` (the Obsidian pipeline), and
`tests/test_notes_e2e.py` (a real WAV and a real Ogg Opus file through the
real endpoint, pipeline and vault).

## Endpoint

```
POST http://<hermes-host>:8766/notes/v1
GET  http://<hermes-host>:8766/health
```

Served by `plugin/notes_ingest.py` (`NotesServer`) in the gateway process, on
its own port — the hub's WebSocket `process_request` sees headers only, so the
note body needs a plain HTTP server beside it. The adapter starts it with the
platform and stops it with the gateway (`GadgetAdapter._notes_retain` /
`_notes_release`); one server serves the whole process even when several
profiles each load their own adapter copy, because the port and the inbox are
device-facing, not per-profile. A bind failure is logged and skipped — a busy
port must not take the gadget platform down. FastAPI and uvicorn ship with
Hermes; without them the server reports the error in the log and the rest of
the plugin works.

Configuration under `platforms.gadget.extra`:

| Key | Default | Meaning |
|---|---|---|
| `notes_port` | `8766` | Upload port; `0` (or `null`) disables the endpoint |
| `notes_host` | the hub's `host` | Bind address |
| `notes_obsidian_dir` | `~/obsidian/Notes/voice notes` | Where notes are written (created with its parents on first write) |

Environment: `GADGET_ACCESS_TOKEN` (or `extra.access_token`) — when set, every
request must also carry it in `X-Gadget-Token`, compared in constant time.

The device key comes from the hub's enrollment, stored base64 in
`<hermes home>/plugin-data/gadget/devices.json` (`~/.hermes/...` for the
default profile; the path follows the active profile). Job storage lives
beside it in `<hermes home>/plugin-data/gadget/notes/`:

```
.staging/<job_id>-*/  transient: recording.<ext> + note.json, renamed away before 202
inbox/<job_id>/       queued:    recording.wav | recording.opus + note.json
done/<job_id>/        processed
failed/<job_id>/      quarantined; .error holds {"code","step","message","attempts"}
```

`GET /health` answers
`{"status":"ok"|"paused","port":N,"last_error":…|null,"inbox":N,"done":M,"failed":K}`.

## Request

`Content-Type: multipart/form-data` with six parts. Every non-file part is
UTF-8 text, matched **exactly as transmitted** (no trimming); unknown parts
are ignored; a repeated part is `400`.

| Part | Required | Format and rules |
|---|---|---|
| `audio` | yes | Exactly one **file** part, non-empty, ≤ 32 MiB. Sniffed from the bytes: WAV PCM16 mono 8–48 kHz, or Ogg Opus mono. The part's own `Content-Type` and `filename` are ignored — send `audio=@file` and the server identifies the codec itself. |
| `device_id` | yes | `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$` — the paired device's id, as in the hub's enrollment. |
| `recorded_at` | yes | ISO-8601 **with an offset**: `2026-11-20T08:15:04-06:00`, `2026-11-20T14:15:04Z`, optional fractional seconds. Normalized to `YYYY-MM-DDTHH:MM:SSZ` for storage and key derivation; the **raw** string goes into the HMAC. Must not be more than 300 s in the future. |
| `duration_seconds` | yes | Decimal integer `1..900`. Advisory: never verified against the audio, copied to the note. |
| `idempotency_key` | no | `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`. Generated once per recording and resent unchanged on every retry; the device's `note_id` fits and is the natural value. |
| `checksum` | no | 64 lowercase hex chars: SHA-256 of the `audio` bytes as sent. When present it must equal the digest the server computed, else `409`. |

Auth headers:

| Header | Rule |
|---|---|
| `X-Gadget-Ts` | Decimal unix seconds. `abs(now − ts) > 300` → `401`. |
| `X-Gadget-Mac` | `base64(HMAC-SHA256(device_key, canonical))`, canonical below. |
| `X-Gadget-Token` | When the host sets `GADGET_ACCESS_TOKEN`: exact match, else `401`. |

```
canonical = hermes-gadget/v1|note|<device_id>|<X-Gadget-Ts>|<recorded_at>|<duration_seconds>|<audio_sha256_hex>[|<idempotency_key>]
```

The MAC covers the field values exactly as transmitted plus the digest of the
bytes the server received, so a truncated or mangled transfer fails auth as
well as checksum. It is the same device key and the same HMAC family as the
hub WebSocket `auth` (`protocol.note_mac`); only the context string differs.
Full rules: contract §2.

### Uploading with curl

Prerequisites: a paired device (so the store has its key), the gadget platform
running, and any WAV or Ogg Opus file — the test fixtures
`tests/fixtures/voice-notes/sample.wav` / `sample.opus` work. From the
repository root:

```bash
# The device's key, base64, from the gadget store
DEVICE=hg-0123456789abcdef
KEY_B64=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["devices"][sys.argv[2]]["key"])' \
  "$HOME/.hermes/plugin-data/gadget/devices.json" "$DEVICE")

# The field values — a device persists these in note.json and resends them
# verbatim on every retry; only the multipart boundary may change.
# recorded_at: ISO-8601 with an offset, at most 300 s in the future; any
# equivalent spelling of the same instant (Z / +00:00 / -06:00) derives the
# same job_id. Two minutes ago always satisfies both rules:
RECORDED_AT=$(python3 -c 'from datetime import datetime, timedelta; print((datetime.now().astimezone() - timedelta(minutes=2)).isoformat(timespec="seconds"))')
DURATION=4
IDEM=hg-0123456789abcdef-20261120T141500-0001   # the device's note_id (any [A-Za-z0-9._-] key ≤ 128
                         # chars); to derive the key on the server instead,
                         # delete this line and the matching
                         # -F idempotency_key line below
AUDIO=tests/fixtures/voice-notes/sample.wav
export DEVICE RECORDED_AT DURATION IDEM AUDIO

# Digest, timestamp and MAC over the raw field strings
eval "$(python3 - "$KEY_B64" <<'PY'
import base64, hashlib, hmac, os, sys, time
key = base64.b64decode(sys.argv[1])
sha = hashlib.sha256(open(os.environ["AUDIO"], "rb").read()).hexdigest()
ts = str(int(time.time()))
parts = [os.environ["DEVICE"], ts, os.environ["RECORDED_AT"], os.environ["DURATION"], sha]
if os.environ.get("IDEM"):
    parts.append(os.environ["IDEM"])
mac = base64.b64encode(hmac.new(key, ("hermes-gadget/v1|note|" + "|".join(parts)).encode(),
                                hashlib.sha256).digest()).decode()
print(f"export SHA={sha!r} TS={ts!r} MAC={mac!r}")
PY
)"

curl -sS -D- -o- -X POST "http://127.0.0.1:8766/notes/v1" \
  -H "X-Gadget-Ts: $TS" -H "X-Gadget-Mac: $MAC" \
  -F "audio=@$AUDIO" \
  -F "device_id=$DEVICE" \
  -F "recorded_at=$RECORDED_AT" \
  -F "duration_seconds=$DURATION" \
  -F "idempotency_key=$IDEM" \
  -F "checksum=$SHA"
```

Expected answer, before any transcription:

```
HTTP/1.1 202 Accepted

{"status":"accepted","job_id":"<64 hex chars>"}
```

Then `curl -s http://127.0.0.1:8766/health` shows the job in `inbox`, and once
the pipeline has run (the profile needs speech-to-text and a model configured)
the note appears under `~/obsidian/Notes/voice notes/` as
`YYYY-MM-DD_HHMM_<device-id>_<short-id>.md` with its audio beside it in
`_audio/`. Sending the exact same command again answers
`200 {"status":"duplicate","job_id":…}` and still writes only one note.

The firmware builds the same request with `hg::crypto` and `esp_http_client`;
a C example is in [the integration spec, §8](../docs/waveshare-epaper-154-v2.md#8-note-upload-contract-firmware--hermes).

## Responses

Success bodies (`202` for both `accepted` shapes, `200` for `duplicate`):

```
{"status": "accepted",  "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b"}
{"status": "accepted",  "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b", "requeued": true}
{"status": "duplicate", "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b"}
```

`job_id` is the server-normalized idempotency key (64 hex). `requeued: true`
appears only when a quarantined job was reopened by this upload.

Error envelope — one shape for every failure, `field` only when one part is
at fault:

```
{"error": {"code": "invalid_field", "message": "duration_seconds must be an integer in 1..900", "field": "duration_seconds", "retryable": false}}
```

| HTTP | `code` | `retryable` | Cause |
|---|---|---|---|
| 400 | `bad_request` | no | not multipart, missing/repeated part, empty or non-file `audio` |
| 401 | `auth` | no | unknown device, `X-Gadget-Ts` skew, MAC mismatch, token mismatch |
| 404 | `not_found` | no | unknown path |
| 405 | `method_not_allowed` | no | not `POST` (the answer keeps `Allow: POST`) |
| 409 | `checksum_mismatch` | no | supplied `checksum` ≠ computed digest |
| 409 | `idempotency_conflict` | no | known `job_id`, different audio bytes |
| 413 | `too_large` | no | audio over 32 MiB or request over audio + 64 KiB |
| 415 | `unsupported_codec` | no | sniffing found no supported codec |
| 422 | `invalid_field` | no | format/range rule failed; `error.field` names the part |
| 500 | `internal` | yes | unexpected server error |
| 503 | `unavailable` | yes | server paused or shutting down |
| 507 | `storage` | yes | staging write failed (e.g. disk full) |
| — | (no response) | yes | timeout, reset, refused |

The response is the **first** failing check in this order (contract §4):
path/method → `Content-Type` → size → parse → field formats → auth → checksum
→ idempotency → staging → `202`. Validation deliberately precedes
authentication, so a malformed request is answered without consulting the
device store.

`retryable` is what the device acts on (contract §3.3): `false` moves the note
to `failed/` and never resends; `true`, or no response at all, resends the
**same bytes and the same field strings** with backoff `5 s, 10 s, 20 s, …`
capped at `300 s`, unbounded while the note sits in `pending/`. Only `202` /
`200` move it to `sent/`.

## Idempotency

`job_id` is always 64 lowercase hex chars (contract §5.1):

```
job_id = sha256hex(device_id + "|" + supplied_idempotency_key)              # when the part was sent
job_id = sha256hex(device_id + "|" + recorded_at_utc + "|" + audio_sha256)  # otherwise
```

`recorded_at_utc` is the normalized `YYYY-MM-DDTHH:MM:SSZ` form, so `Z`,
`+00:00` and an equivalent `-06:00` derive the same key. The short id used in
filenames is `job_id[:8]`.

| Stored state | Audio bytes | Answer |
|---|---|---|
| none | — | store, `202 accepted` |
| `inbox/` (queued or in flight) | same | `200 duplicate` |
| `done/` | same | `200 duplicate` |
| `failed/` (quarantined) | same | re-enqueue: `attempts` reset, `.error` removed, back to `inbox/` → `202 accepted` with `requeued: true` |
| any | different | `409 idempotency_conflict` |

A supplied key deliberately excludes the content: a device that re-sends one
key with *different* audio is buggy and gets `409` instead of a second note.
Two *different* `job_id`s carrying the same recording (once with a supplied
key, once without) create two jobs but still one note: before writing, the
pipeline scans the frontmatter of `voice notes/*.md` and closes the job if one
already carries this `idempotency_key`, or the same
(`device_id`, `recorded_at_utc`, `audio_sha256`) triple (contract §5.7).

A note deleted from the vault is restored by moving `done/<job_id>/` back to
`inbox/<job_id>/` and setting `attempts` to `0` in its `note.json`; the dedupe
scan then finds nothing and rewrites it.

## Notes in the vault

- **Folder**: `voice notes` — lowercase, one space. On a case-sensitive
  filesystem `Voice Notes` would be a *different* folder and split the vault
  in two; the prototype's spelling is gone. Default `notes_obsidian_dir` is
  `~/obsidian/Notes/voice notes`, created with its parents on first write.
- **Filename**: `YYYY-MM-DD_HHMM_<device-id>_<short-id>.md`, e.g.
  `2026-11-20_0815_hg-0123456789abcdef_3f9a1c7d.md` — `recorded_at` converted
  to the **host's local timezone** (DST-aware), the device id as sent, and
  `job_id[:8]`, so a retry rebuilds the identical name.
- **Audio**: copied byte-identical beside the notes as
  `_audio/<same stem>.wav` or `_audio/<same stem>.opus`; the frontmatter
  `audio:` link is relative to the note: `_audio/<file>`.
- **Frontmatter**: twelve keys in a fixed order (`idempotency_key`,
  `device_id`, `recorded_at`, `duration_seconds`, `codec`, `audio_sha256`,
  `audio`, `title`, `transcript`, `summary`, `key_points`, `tags`), then a
  body of `# <title>`, `## Summary`, `## Key points`, `## Transcript`. The
  transcript is verbatim STT output and is never rewritten. Full format:
  contract §5.6.
- **Writes are atomic**: the audio is written first (tmp + fsync +
  `os.replace`), then the markdown (tmp + fsync + `os.replace` — the commit
  point). A crash at any point leaves either nothing or a complete note; the
  idle pass sweeps `.tmp-*` debris older than 24 h (contract §5.7).

## Retries and failures

**Device side** (contract §3.3): only `retryable` answers are retried — same
bytes, same field strings, backoff `5 s ×2` capped at `300 s`, unbounded
while the note sits in `pending/`; any `4xx` moves it to `failed/`.

**Host pipeline** (contract §5.8): one worker thread per server, woken by an
upload or by the 120 s idle poll; a worker never dies on one bad note.

- *Retryable* — STT or model connect/timeout errors, HTTP `429`/`5xx`, empty
  or unparseable model output, vault `ENOSPC`/`EAGAIN`/`EBUSY`/`EINTR`, and
  anything unclassified. Recorded in `note.json` (`attempts`, `last_error`)
  and retried after `min(30 s × 2^(n−1), 3600 s)` → **30 s, 1 m, 2 m, 4 m,
  8 m, 16 m, 32 m**.
- *Terminal* — quarantined immediately: provider `4xx` other than `429`,
  missing or invalid credentials/config, undecodable audio, staged bytes that
  no longer match `audio_sha256`, an empty transcript, vault
  `EACCES`/`EPERM`/`EROFS`/`EINVAL`/`ENOENT`.
- **8 attempts** total (1 + 7 retries); the eighth failure quarantines with
  `code: attempts_exhausted`. Either way the entry moves to `failed/` with
  `.error` = `{"code","step":"stt|summarize|write|verify","message","attempts"}`.

**Recovery**: re-upload the same idempotency key — the endpoint resets
`attempts`, removes `.error`, moves the entry back to `inbox/` and answers
`202` with `requeued: true`. Or move `failed/<job_id>/` back to
`inbox/<job_id>/` by hand and set `attempts` to `0` in its `note.json` (an
entry whose `attempts` still reads 8 is re-quarantined on the next pass).

**Staging** (inside the request): transient write errors (`EINTR`, `EAGAIN`,
`EBUSY`) are retried three times, 100 ms apart, before answering `507`;
`ENOSPC` is not retried in-request — `507` and the device's backoff handle it.

## Troubleshooting

| Symptom | Check | Fix |
|---|---|---|
| curl: connection refused | Is the gadget platform enabled? `notes_port: 0` disables the endpoint; a busy port is logged (`voice-note endpoint not started …`) and skipped rather than crashing the platform | Set `notes_port`, free the port, restart the gateway |
| 404 from `:8765` or the dashboard | The notes endpoint is its own server (`notes_port`, default 8766), not the hub WebSocket port or the dashboard | Use `http://<host>:8766/notes/v1` |
| `401 auth` | Device not enrolled (id ↔ key), `X-Gadget-Ts` outside ±300 s (board RTC), MAC over different strings — the canonical string must use the raw transmitted field values and include `\|idempotency_key` **only** when that part was sent — or a token mismatch | Re-pair the device; fix the clock; recompute over exactly the transmitted fields |
| `409 checksum_mismatch` | The `checksum` part ≠ the bytes received: a corrupted transfer | Resend the identical audio |
| `409 idempotency_conflict` | Same `job_id`, different audio: the firmware re-used a key for another recording | Generate `idempotency_key` once per recording and never change it |
| `422 invalid_field` | `error.field` names the part — `recorded_at` needs an offset and ≤ 300 s in the future; `duration_seconds` must be `1..900` | Fix that field |
| `415 unsupported_codec` | Not WAV PCM16 mono 8–48 kHz and not Ogg Opus mono (sniffed from bytes, not the declared type) | Re-encode the recording |
| `413 too_large` | Audio > 32 MiB, or the whole request > audio cap + 64 KiB | Trim the recording |
| `503 unavailable` | The server is paused or shutting down (`"status":"paused"` in `/health`) | Retry; it answers again after the restart |
| `507 storage` | Host disk full or the staging directory unwritable; `/health` `last_error` says what failed | Free space — the device's backoff resends automatically |
| `202` but no note appears | `GET /health`: is the job still in `inbox`? In `failed/` its `.error` names `code`, `step` and `attempts`. The profile needs speech-to-text **and** a model configured — both run host-side after the 202 | Configure STT/model, then re-upload the same key (that requeues a quarantined job), or fix what `.error` names |
| Note lands in the wrong folder | `notes_obsidian_dir` default and case — the canonical folder is `voice notes` | Set `notes_obsidian_dir`; fix the spelling |
| Duplicate-looking notes after hand edits | Dedupe reads frontmatter `idempotency_key`; stripping frontmatter removes the key | Restore the frontmatter, or leave the note alone |

## Tests

From the repository root, in any environment with the dev extras installed
(`pip install -e .[dev]`; `pytest` itself is all they need):

```bash
pytest tests/test_notes_ingest.py tests/test_notes_pipeline.py tests/test_protocol.py -q   # wire contract, pipeline, MAC vectors
pytest tests/test_notes_e2e.py -q                                   # the integration test
pytest tests/test_notes_e2e.py -q -k opus                           # one codec
```

`tests/test_notes_e2e.py` uploads the committed real-audio fixtures
(`tests/fixtures/voice-notes/sample.wav` and `sample.opus`; regeneration is in
that folder's README) through the real `NotesServer` and the real
`NotesPipeline` into a temporary vault — 22 cases covering both codecs, both
transports, the rejection matrix, idempotency, backoff and quarantine.
Nothing touches your `~/obsidian`, and no environment variables are needed:
only STT and the summary model are doubles (they cannot run in CI). The
socket transport runs where `fastapi`/`uvicorn` are present (they ship with
Hermes) and skips where they are not; the `direct` transport then exercises
the identical server and pipeline code.

The contract and pipeline suites need FastAPI only for the route cases;
everything else runs without it, and without a Hermes checkout.
