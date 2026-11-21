# Voice-note upload contract and ingestion

Normative contract for offline voice notes: how firmware uploads a finished
recording to Hermes, and what Hermes does with it until it is a note in the
vault. Firmware authors implement [§2](#2-request)–[§3](#3-responses); the host
implements [§4](#4-validation-order)–[§6](#6-configuration). The
firmware's own half — the SD folders `pending/` `sent/` `failed/`, its
`note.json`, the uploader loop — stays in
[the integration spec, §7](waveshare-epaper-154-v2.md); its §8 request example
is replaced by this document: uploads are `multipart/form-data`, not an
octet-stream body with metadata in `X-Gadget-*` headers.

Everything below is fixed: field names, formats, status codes, backoff
numbers, filenames. An implementation conforms or it is wrong — nothing is
left for the implementer to choose. The host implementation is
`plugin/notes_ingest.py` (endpoint) and `plugin/notes_pipeline.py`
(pipeline); `plugin/NOTES.md` is the endpoint's API reference and runbook
(curl example, response codes, troubleshooting, test commands) and
[§7](#7-relationship-to-the-earlier-sketches) lists what changed from the
prototype they grew from.

## 1. Endpoint

```
POST http://<hermes-host>:8766/notes/v1
GET  http://<hermes-host>:8766/health
```

- Port `notes_port` (default `8766`; `0` disables the endpoint), bind
  `notes_host` (default: the hub's host). Served by the gadget plugin's notes
  server inside the gateway process, plain HTTP: this is a LAN/tailnet
  endpoint. The HMAC below authenticates and integrity-protects, it does not
  encrypt.
- Unknown path → `404`; a method other than `POST` → `405` with
  `Allow: POST`.
- `GET /health` →
  `{"status":"ok"|"paused","port":N,"last_error":…|null,"inbox":N,"done":M,"failed":K}`.
- A bind failure is logged and the endpoint stays down; the rest of the
  gadget platform keeps running.

## 2. Request

`Content-Type: multipart/form-data; boundary=…`. Six parts. Every non-file
part is UTF-8 text, matched **exactly as transmitted** (no trimming); unknown
parts are ignored; a repeated part is an error. The device persists these
field values verbatim in its `note.json` next to the audio file, so every
retry of one recording is byte-identical.

| Part | Required | Format and rules |
|---|---|---|
| `audio` | yes | Exactly one **file** part (it carries a `filename`; a text-typed `audio` part is `400`). Non-empty, ≤ 32 MiB. Codec per [§2.3](#23-audio-codecs); the part's own `Content-Type` and `filename` are advisory — the server sniffs the bytes. |
| `device_id` | yes | `^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$` — the paired device's id, as in the hub's enrollment. |
| `recorded_at` | yes | ISO-8601 **with an offset**: `2026-11-20T08:15:04-06:00`, `2026-11-20T14:15:04Z`, optional fractional seconds, offset `±hh:mm` or `±hhmm`. Naive (offset-less) timestamps are rejected. The server normalizes it to UTC whole seconds, `YYYY-MM-DDTHH:MM:SSZ`, for storage and key derivation; the **raw** string goes into the HMAC and stays in `note.json`. Must not be more than 300 s in the future; there is no lower bound. |
| `duration_seconds` | yes | Decimal integer in `1..900`. Advisory: never verified against the audio (the server does not decode it), copied to the note. |
| `idempotency_key` | no | `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`. Generated once per recording by the device and resent unchanged on every retry; [§5.1](#51-idempotency-key-derivation) defines the key with and without it. |
| `checksum` | no | 64 lowercase hex chars: SHA-256 of the `audio` bytes as sent. When present it must equal the digest the server computes, else `409`. The server always computes the digest itself; this field only lets a corrupted transfer be rejected loudly. |

A missing required part, a repeated part, or an empty/non-file `audio` part is
`400`. A value that fails its format or range rule is `422`, and
`error.field` names the part.

### 2.1 Auth headers

| Header | Required | Rule |
|---|---|---|
| `X-Gadget-Ts` | yes | Decimal unix seconds. `abs(now − ts) > 300` → `401`. |
| `X-Gadget-Mac` | yes | `base64(HMAC-SHA256(device_key, canonical))`, canonical string per [§2.2](#22-canonical-string). |
| `X-Gadget-Token` | when `GADGET_ACCESS_TOKEN` is set on the host | Exact match, compared in constant time, else `401`. |

`device_key` is the paired device's key from hub enrollment — the same key and
HMAC shape as the hub WebSocket `auth` (`protocol.auth_mac`); only the context
string differs. A well-formed `device_id` with no key on file → `401`.

### 2.2 Canonical string

```
hermes-gadget/v1|note|<device_id>|<X-Gadget-Ts>|<recorded_at>|<duration_seconds>|<audio_sha256_hex>[|<idempotency_key>]
```

- Field values are the raw bytes as transmitted — untrimmed, unnormalized —
  so the device can compute the MAC before it knows how the server will
  normalize anything.
- `audio_sha256_hex` is the lowercase hex digest of the `audio` part: the
  device computes it over the bytes it sends, the server over the bytes it
  received.
- The final `|<idempotency_key>` segment is present only when that part was
  sent.
- A retry resends the same field strings and the same audio bytes; only the
  multipart boundary may change.

### 2.3 Audio codecs

Sniffed from magic bytes; the declared part type is ignored.

- **WAV**: `RIFF` … `WAVE`, format tag 1 (PCM), **16-bit, mono**, 8000–48000
  Hz (the boards record at 24000 Hz).
- **Opus**: Ogg container (`OggS` + `OpusHead`), **mono**, any rate.
- Anything else — MP3, AAC, FLAC, raw PCM, stereo or float WAV → `415`
  `unsupported_codec`.

### 2.4 Limits

- Audio part ≤ **32 MiB** (33 554 432 bytes). Whole request ≤ audio cap +
  64 KiB: checked from `Content-Length` before the body is read, and enforced
  while reading when the header is absent (then `413` and the connection is
  dropped).
- `duration_seconds` ∈ `1..900` — 15 min of headroom over the boards' 5 min
  recording cap.
- `recorded_at` has no lower bound (a late upload of an old recording is
  legal), so the server cannot tell a real date from a dead clock: **the sender
  owns the clock**. A board with no RTC boots at 1970, and the reference
  firmware therefore uploads a note it recorded before its first clock sync
  stamped with the time it sends it, never with 1970
  ([firmware modules](firmware-modules.md#notesmode)). The server neither
  reorders nor rejects a note on an old stamp, so the sender is the only thing
  that can get this right.

## 3. Responses

### 3.1 Success

```
{"status": "accepted",  "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b"}
{"status": "accepted",  "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b", "requeued": true}
{"status": "duplicate", "job_id": "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b"}
```

HTTP `202` for both `accepted` shapes, `200` for `duplicate`. `job_id` is the
server-normalized idempotency key ([§5.1](#51-idempotency-key-derivation),
always 64 hex chars). `requeued: true` appears only when a quarantined job was
reopened ([§5.3](#53-duplicate-handling)).

**`202` is sent as soon as the audio is durably staged**, before any
transcription. The device never waits on a model; everything after the store
is a host-side concern.

### 3.2 Errors

```json
{"error": {"code": "invalid_field", "message": "duration_seconds must be an integer in 1..900", "field": "duration_seconds", "retryable": false}}
```

`field` appears only when one part is at fault; `retryable` mirrors the table.

| HTTP | `code` | `retryable` | Cause | Device does |
|---|---|---|---|---|
| 400 | `bad_request` | no | not multipart, missing/repeated part, empty or non-file `audio` | → `failed/` |
| 401 | `auth` | no | unknown device, bad/absent MAC, token mismatch, `X-Gadget-Ts` skew | → `failed/` |
| 404 | `not_found` | no | unknown path | → `failed/` |
| 405 | `method_not_allowed` | no | not `POST` | → `failed/` |
| 409 | `checksum_mismatch` | no | supplied `checksum` ≠ computed digest | → `failed/` |
| 409 | `idempotency_conflict` | no | known `job_id`, different audio bytes | → `failed/` |
| 413 | `too_large` | no | audio or request over the cap | → `failed/` |
| 415 | `unsupported_codec` | no | sniffing found no supported codec | → `failed/` |
| 422 | `invalid_field` | no | format/range rule failed, or `recorded_at` too far in the future | → `failed/` |
| 500 | `internal` | yes | unexpected server error | retry |
| 503 | `unavailable` | yes | server paused or shutting down | retry |
| 507 | `storage` | yes | staging write failed (e.g. disk full) | retry |
| — | (no response) | yes | timeout, reset, refused | retry |

### 3.3 Device retry rules

- `retryable: false` (any 4xx) → move the note to `failed/`, never resend it.
- `retryable: true`, or no response at all → resend the **same bytes and the
  same field strings** with backoff `5 s, 10 s, 20 s, …` capped at `300 s`
  (×2 per attempt, no jitter), attempts unbounded while the note sits in
  `pending/`. Only `202`/`200` move it to `sent/`. Reboots, battery pulls and
  Wi-Fi flaps therefore converge on "uploaded exactly once": the idempotency
  key is derived from what the note *is*, not from the attempt.

## 4. Validation order

The response is the **first** failing check, in this order:

1. path/method → `404`/`405`
2. `Content-Type` → `400`
3. size pre-check → `413`
4. parse: required parts present, no repeats, `audio` is a non-empty file
   part → `400`
5. field formats and ranges → `422`
6. auth (device key, ts skew, MAC, token) → `401`
7. `checksum` verify → `409`
8. idempotency lookup ([§5.3](#53-duplicate-handling)) → `200`/`409`/`202`
9. staging store → `507`/`503`
10. `202 accepted`

Validation precedes authentication on purpose: nothing is read or written
before step 6 beyond the size-bounded body, the MAC covers the raw field
values no matter what they contain, and contract tests for steps 4–5 need no
device key.

## 5. Ingestion on the host

### 5.1 Idempotency key derivation

`job_id` is always 64 lowercase hex chars:

```
job_id = sha256hex(device_id + "|" + supplied_idempotency_key)     # when the part was sent
job_id = sha256hex(device_id + "|" + recorded_at_utc + "|" + audio_sha256)   # otherwise
```

- `recorded_at_utc` is the normalized `YYYY-MM-DDTHH:MM:SSZ` form, so `Z`,
  `+00:00` and an equivalent `-06:00` derive the same key.
- A supplied key deliberately excludes the content: if a device re-sends one
  key with *different* audio, that is a firmware bug and surfaces as `409`
  `idempotency_conflict` instead of silently creating a second note.
- The short id used in filenames is `job_id[:8]`.

### 5.2 Job storage

Accepted jobs live under `~/.hermes/plugin-data/gadget/notes/`:

```
.staging/<job_id>-*/ transient: recording.<ext> + note.json, renamed away before 202
inbox/<job_id>/      queued:    recording.wav | recording.opus + note.json
done/<job_id>/       processed
failed/<job_id>/     quarantined; .error holds {"code","step","message","attempts"}
```

`note.json` carries `job_id`, `device_id`, the raw and normalized
`recorded_at`, `duration_seconds`, `audio_sha256`, `codec`, `supplied_key`
(or `null`), `attempts`, `last_error`, `accepted_unix`.

### 5.3 Duplicate handling

For a request whose `job_id` already exists:

| Stored state | Audio bytes | Answer |
|---|---|---|
| none | — | store, `202 accepted` |
| `inbox/` (queued or in flight) | same | `200 duplicate` |
| `done/` | same | `200 duplicate` |
| `failed/` (quarantined) | same | re-enqueue: reset `attempts`, clear `.error`, move back to `inbox/` → `202 accepted` with `requeued: true` |
| any | different | `409 idempotency_conflict` |

- Check-and-store for one `job_id` is serialized in-process. Two concurrent
  uploads race only on the final directory rename; the loser re-reads the
  stored `note.json` — same digest → `200 duplicate`, different → `409`.
- Two *different* `job_id`s carrying the same recording (once with a supplied
  key, once without) create two jobs but still one note: the pipeline
  deduplicates on frontmatter before writing ([§5.7](#57-atomicity)).
- A note deleted from the vault is restored by moving `done/<job_id>/` back
  to `inbox/<job_id>/` (attempts reset); the dedupe scan then finds nothing
  and rewrites it.

### 5.4 Obsidian destination

- Canonical folder: **`voice notes`** — lowercase, one space — created with
  its parents on first write. Default `notes_obsidian_dir` is
  `~/obsidian/Notes/voice notes`. (This replaces the prototype's
  `Voice Notes`; on a case-sensitive filesystem two spellings would split the
  vault in two.)
- The audio copy lives beside the notes under `_audio/`, and the `audio:`
  frontmatter link is relative to the note: `_audio/<file>`.

### 5.5 Filename

```
<YYYY-MM-DD>_<HHMM>_<device-id>_<short-id>.md
2026-11-20_0815_hg-0123456789abcdef_3f9a1c7d.md
```

- Date and time: `recorded_at_utc` converted to the **host's local
  timezone** (DST-aware), zero-padded `YYYY-MM-DD`, 24-hour `HHMM`. The
  example above is `14:15:04Z` on a CST host.
- That date is the device's word for it, and a board with no RTC is trusted to
  know better than 1970: one that recorded the note before its first clock sync
  sends the time it finally uploaded instead ([§2.4](#24-limits)), so such a
  note is dated when it was sent. Nothing here tries to guess when it was
  spoken.
- `device-id` is `device_id` as sent — its charset makes it filename-safe.
- `short-id` is `job_id[:8]`: deterministic, so a retry or a restart after a
  crash rebuilds the identical name. The audio copy is
  `_audio/<same stem>.wav` or `_audio/<same stem>.opus`.
- Collision — same name but the existing file's frontmatter carries a
  different `idempotency_key` (a 32-bit short-id clash) — append `-2`, `-3`,
  … before `.md` until the name is free, use the same stem for the audio
  copy, and log it.

### 5.6 Note format

Frontmatter keys, in this order, all required:

```yaml
---
idempotency_key: 3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b
device_id: hg-0123456789abcdef
recorded_at: 2026-11-20T14:15:04Z
duration_seconds: 8
codec: wav
audio_sha256: b6d81b360a5672d80c27430f39153e2c470971a86ad1c25a6d17d381a56f6c21
audio: _audio/2026-11-20_0815_hg-0123456789abcdef_3f9a1c7d.wav
title: Pick up the dry cleaning
transcript: |
  …verbatim speech-to-text output…
summary: |
  …one to three sentences…
key_points:
  - …
tags: [voice-note]
---
```

Body, in this order: `# <title>`, `## Summary` (the summary text), `## Key
points` (the list), `## Transcript` (the verbatim transcript).

- The transcript is never rewritten: the summary may reorganize what was
  said, the transcript keeps exactly what STT returned.
- `title` ≤ 60 characters, model-written.
- `transcript` and `summary` appear both in the frontmatter and in the body:
  the frontmatter is what tests and tools read, the body is what a reader
  sees.

### 5.7 Atomicity

Upload:

1. Read, bound and verify the whole request first — a device that dies
   mid-body leaves nothing behind.
2. Write `recording.<ext>` and `note.json` into `.staging/<job_id>-*/`, fsync
   both files and the directory, then `rename()` the directory into
   `inbox/<job_id>/`. Staging and inbox share a volume, so the rename is
   atomic: an inbox entry is complete or absent, never partial.
3. Transient write errors (`EINTR`, `EAGAIN`, `EBUSY`) are retried three
   times, 100 ms apart, inside the request before answering `507`. `ENOSPC`
   is not retried in-request — `507` and the device's backoff handle it.
   Waking the worker cannot fail the request: the job is already durable and
   the idle poll (120 s) picks it up.

Note write, in this order:

1. **Dedupe scan** — read the frontmatter of `voice notes/*.md`; if one
   already carries this `idempotency_key`, or the same (`device_id`,
   `recorded_at_utc`, `audio_sha256`), skip the write and close the job as
   done.
2. Copy the audio to `_audio/<stem>.<ext>.tmp-<short-id>`, fsync, `os.replace`
   onto the final name.
3. Render the markdown to `<stem>.md.tmp-<short-id>` in the target folder,
   fsync, `os.replace` onto the final name. **This replace is the commit
   point.**
4. fsync the target directory (best effort; ignore filesystems that refuse
   it).
5. `rename(inbox/<job_id>/ → done/<job_id>/)`.

Consequences: a failure before step 3 leaves no `.md` — temp files are removed
on failure, and the idle pass sweeps `.tmp-*` older than 24 h (crash debris).
A crash between steps 3 and 5 leaves a complete note plus an inbox entry; the
next pass dedupes at step 1 and closes the job, so it never writes a second
note. Audio is written before the markdown: an orphaned audio copy is harmless
and gets replaced idempotently, a note without its audio would not be.

### 5.8 Pipeline retries and the error taxonomy

One worker thread per server, one pass at a time, woken by an upload, by a
finished job, or by the 120 s idle poll. Any exception is caught: one bad job
is recorded and the pass continues — a worker never dies on a note.

**Retryable** — schedule another pass, count the attempt:

- STT or model provider: connect/timeout errors, HTTP `429`/`5xx`, empty or
  unparseable model output.
- Vault write: `ENOSPC`, `EAGAIN`, `EBUSY`, `EINTR`.
- Anything unclassified: unknown failures retry too, bounded by the attempt
  cap below.

**Terminal** — quarantine immediately, no retry:

- Provider `4xx` other than `429`; missing or invalid credentials/config.
- Unsupported or undecodable audio; staged bytes that no longer match
  `audio_sha256` (corruption).
- An empty transcript — nothing was said, retrying cannot help.
- Vault `EACCES`, `EPERM`, `EROFS`, `EINVAL` (path unusable).
- The attempt cap (below).

**Backoff**: the delay before retry *n* (`n = 1…7`) is
`min(30 s × 2^(n−1), 3600 s)` → 30 s, 1 m, 2 m, 4 m, 8 m, 16 m, 32 m.
**8 attempts** total (1 + 7) per job; the eighth failure quarantines with
`{"code": "attempts_exhausted", …}`. A terminal failure writes `.error` with
`{"code", "step": "stt|summarize|write|verify", "message", "attempts"}`.

**Recovery** resets `attempts` to 0: the device re-uploads the same
idempotency key ([§5.3](#53-duplicate-handling)), or an operator moves
`failed/<job_id>/` back to `inbox/<job_id>/`.

## 6. Configuration

Under `platforms.gadget.extra`:

| Key | Default | Meaning |
|---|---|---|
| `notes_port` | `8766` | Upload port; `0` or `null` disables the endpoint (logged; the platform is unaffected) |
| `notes_host` | the hub's `host` | Bind address |
| `notes_obsidian_dir` | `~/obsidian/Notes/voice notes` | Where notes are written |

Environment: `GADGET_ACCESS_TOKEN` — if set, every request must carry it in
`X-Gadget-Token`.

Fixed constants: audio cap 32 MiB; `duration_seconds` `1..900`; clock skew
±300 s for both `X-Gadget-Ts` and a future `recorded_at`; pipeline attempts 8
with the backoff above; device backoff 5 s → 300 s; idle poll 120 s; staging
retry ×3 at 100 ms.

## 7. Relationship to the earlier sketches

[The integration spec's §8](waveshare-epaper-154-v2.md) carries the firmware's
view of this contract — request example, C upload example, response table,
uploader loop — and `plugin/NOTES.md` is the host-side API reference and
runbook. Both follow this document; they replaced a prototype with the same
port, same HMAC family, same staging and `202`-before-STT shape, but an
octet-stream body with metadata in `X-Gadget-*` headers, a device-chosen
`note_id`, WAV only, the `Voice Notes` folder and slug filenames. This
document is normative for the request format, idempotency, filenames and
retries. What changed in code from that prototype:

| Prototype | This contract |
|---|---|
| octet-stream body + `X-Gadget-Device`/`Note-Id`/`Duration-Ms`/`Sample-Rate`/`Sha256` headers | multipart parts `audio`, `device_id`, `recorded_at`, `duration_seconds`, optional `idempotency_key`, `checksum` |
| idempotency = device-supplied `note_id` | supplied key, else `sha256hex(device_id\|recorded_at\|sha256)` |
| WAV PCM16 only | WAV PCM16 mono + Ogg Opus mono |
| `Voice Notes/` + `YYYY-MM-DD HHMMSS <slug>.md` | `voice notes/` + `YYYY-MM-DD_HHMM_<device-id>_<short-id>.md` |
| `{"error": "code"}` | `{"error": {code, message, field, retryable}}` |
| fixed 30/120/600/3600 s ladder, 5 attempts | `30 s × 2` capped at 3600 s, 8 attempts |
| frontmatter `note_id` dedupe | `idempotency_key`, or (`device_id`, `recorded_at`, `audio_sha256`) dedupe |

## 8. Conformance

Tests pinning this contract must exercise, end to end:

- every required part missing in turn → `400` with `error.field`; bad
  `recorded_at`/`duration_seconds`/charset → `422`; unknown device or bad MAC
  → `401`; oversize → `413`; non-WAV/non-Opus bytes → `415`; wrong
  `checksum` → `409`;
- a real WAV fixture and a real Ogg Opus fixture, each producing exactly one
  note under `voice notes/` with the filename of [§5.5](#55-filename) and the
  frontmatter of [§5.6](#56-note-format);
- the same idempotency key uploaded twice → `202` then `200 duplicate`, still
  exactly one note; a supplied key with different bytes → `409`;
- a forced transient failure (STT down, vault read-only) → backoff schedule,
  `attempts` counting, and quarantine into `failed/` after the eighth;
- atomicity: a failure mid-write leaves no `.md`, and a crash after the
  markdown replace closes the job on the next pass without a second note.
