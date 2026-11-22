"""End-to-end conformance test for the voice-note ingestion contract.

``docs/voice-note-ingestion.md`` §8 asks for exactly this: a real sample WAV
and a real Ogg Opus fixture, each uploaded through the endpoint, transcribed,
summarized and written into an Obsidian-shaped vault — plus the validation
matrix, the idempotency matrix and a forced retryable processing failure.

What runs
---------
* the real ``NotesServer`` (multipart parsing, HMAC, checksum, idempotency
  ledger, durable staging), over a **real HTTP socket** when the Hermes ASGI
  dependencies are importable, or through ``NotesServer.handle_upload`` — the
  same server code minus uvicorn — when they are not;
* the real ``NotesPipeline`` worker, retry bookkeeping and vault writer;
* a temporary vault (``tmp_path``): nothing touches the developer's real
  ``~/obsidian``;
* the two services that cannot run in CI are doubles, and nothing else is:
  retrieval/STT (``Doubles.transcribe``, which also asserts it is handed the
  exact staged bytes) and the summary model (``Doubles.summarize``).

Fixtures (checked in, real audio — the same 3.4 s spoken sentence in both
containers, so no ffmpeg, TTS, model or network is needed at test time):

===============================  ============================================
``tests/fixtures/voice-notes/sample.wav``
    24 kHz mono PCM16 WAV, 165 794 B, sha256 ``526ea0b3e1f577df1e1c7e40f20c6651af621dc7b7ed4d06cbbc81fbb670eb7f``
``tests/fixtures/voice-notes/sample.opus``
    mono Ogg Opus (16 kHz input rate), 9 653 B, sha256 ``74318cfb4595e1bb0c31b2737e768bef829f0461813737d6588ff66d63beaab7``
===============================  ============================================

How to regenerate them is in ``tests/fixtures/voice-notes/README.md``.

Environment variables
---------------------
None required. The test uses only ``tmp_path`` and ``127.0.0.1``. No
``HERMES_AGENT_DIR`` and no Hermes home are needed, because both model calls
are injected. If ``fastapi``/``uvicorn`` are absent (a plain
``pip install -e .[dev]`` CI job) the two ``socket`` parametrizations skip and
the ``direct`` ones still exercise the identical server and pipeline code;
they are present wherever Hermes is installed, which is where the endpoint
actually runs.

Run it
------
    pytest tests/test_notes_e2e.py -q          # from the repository root
    pytest tests/test_notes_e2e.py -q -k opus # one codec
"""

from __future__ import annotations

import hashlib
import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import pytest

from hermes_gadget_plugin import notes_pipeline, protocol
from hermes_gadget_plugin.notes_ingest import MAX_REQUEST_BYTES, NotesServer
from hermes_gadget_plugin.store import DeviceStore

# -- fixed inputs (the device's representative metadata) -------------------------------

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "voice-notes"
SAMPLE = {"wav": FIXTURES / "sample.wav", "opus": FIXTURES / "sample.opus"}

KEY = bytes(range(32))
DEVICE = protocol.device_id_for_key(KEY)  # hg-<sha256(key)[:16]>, the enrolled shape
OTHER_DEVICE = "hg-00000000000000ff"  # well-formed, never enrolled

#: Raw field values as the device transmits them: the MAC covers these exact
#: bytes (§2.2), and ``note.json`` keeps them next to the normalized stamp.
RECORDED_AT = "2026-11-20T08:15:04-06:00"
RECORDED_AT_UTC = "2026-11-20T14:15:04Z"
DURATION = "3"  # advisory, never checked against the audio (§2)
IDEMPOTENCY_KEY = "hg-630dcd2966c43366-20261120T141504Z-0001"

TRANSCRIPT = "Call the plumber tomorrow about the kitchen sink leak before noon."
TITLE = "Call the plumber about the kitchen sink leak"
NOTE_SLUG = "Call the plumber about the kitchen sink leak"
SUMMARY = "A kitchen sink leak needs a plumber tomorrow before noon."
KEY_POINTS = ("Call the plumber tomorrow", "Mention the kitchen sink leak before noon")

BOUNDARY = "----hermesGadgetE2EBoundary7d01"
NOTE_PATH = "/notes/v1"
HEALTH_PATH = "/health"

#: §5.8, pinned here so a drift in the schedule fails this test, not silently.
BACKOFF_S = (30, 60, 120, 240, 480, 960, 1920)
MAX_ATTEMPTS = 8


def job_id_for(device: str = DEVICE, *, key: str | None = IDEMPOTENCY_KEY, audio: bytes | None = None) -> str:
    """§5.1, recomputed from the contract rather than read back from the server."""
    if key is not None:
        seed = f"{device}|{key}"
    else:
        assert audio is not None
        seed = f"{device}|{RECORDED_AT_UTC}|{hashlib.sha256(audio).hexdigest()}"
    return hashlib.sha256(seed.encode()).hexdigest()


JOB_ID = job_id_for()


def expected_note_name(job_id: str, device: str = DEVICE) -> str:
    """§5.5: recorded_at_utc in the host's local zone, then the note's title."""
    when = datetime.fromisoformat(RECORDED_AT_UTC.replace("Z", "+00:00")).astimezone()
    return f"{when:%Y-%m-%d} {NOTE_SLUG}.md"


# -- multipart wire building ------------------------------------------------------------


@dataclass
class Part:
    name: str
    value: bytes
    filename: str | None = None
    content_type: str | None = None

    def render(self, boundary: str) -> bytes:
        head = f'--{boundary}\r\nContent-Disposition: form-data; name="{self.name}"'
        if self.filename is not None:
            head += f'; filename="{self.filename}"'
        body = head.encode("utf-8") + b"\r\n"
        if self.content_type:
            body += f"Content-Type: {self.content_type}\r\n".encode("utf-8")
        return body + b"\r\n" + self.value + b"\r\n"


def multipart(parts: list[Part], boundary: str = BOUNDARY) -> bytes:
    return b"".join(part.render(boundary) for part in parts) + f"--{boundary}--\r\n".encode()


def upload_parts(audio: bytes, codec: str, *, device: str = DEVICE, recorded_at: str = RECORDED_AT,
                 duration: str = DURATION, idem: str | None = IDEMPOTENCY_KEY,
                 checksum: str | None = None, audio_as_text: bool = False,
                 audio_twice: bool = False, drop: tuple[str, ...] = ()) -> list[Part]:
    """The six parts of §2 in wire order, with the knobs the negative tests need."""
    parts = [Part("audio", audio,
                  filename=None if audio_as_text else f"sample.{codec}",
                  content_type=None if audio_as_text else f"audio/{codec}")]
    if audio_twice:
        parts.append(Part("audio", audio, filename=f"sample-again.{codec}"))
    if "device_id" not in drop:
        parts.append(Part("device_id", device.encode()))
    if "recorded_at" not in drop:
        parts.append(Part("recorded_at", recorded_at.encode()))
    if "duration_seconds" not in drop:
        parts.append(Part("duration_seconds", duration.encode()))
    if idem is not None:
        parts.append(Part("idempotency_key", idem.encode()))
    if checksum is not None:
        parts.append(Part("checksum", checksum.encode()))
    return parts


def signed_headers(audio: bytes, *, device: str = DEVICE, recorded_at: str = RECORDED_AT,
                   duration: str = DURATION, idem: str | None = IDEMPOTENCY_KEY,
                   key: bytes = KEY, ts: str | None = None, mac: str | None = None) -> dict[str, str]:
    ts = ts if ts is not None else str(int(time.time()))
    audio_sha = hashlib.sha256(audio).hexdigest()
    if mac is None:
        mac = protocol.note_mac(key, device, ts, recorded_at, duration, audio_sha, idem)
    return {"X-Gadget-Ts": ts, "X-Gadget-Mac": mac}


# -- transports: real socket, or the same server code without uvicorn -------------------


class DirectTransport:
    """``NotesServer.handle_upload`` — §4's whole contract minus the byte-moving layer.

    Always available: it is the path the server runs once uvicorn has the body,
    so the test still covers validation, auth, the ledger, staging and the
    pipeline in a bare CI job.
    """

    name = "direct"

    def __init__(self, server: NotesServer) -> None:
        self.server = server

    def post(self, headers: dict[str, str], content_type: str, content_length: int | None,
             body: bytes) -> tuple[int, dict]:
        return self.server.handle_upload(headers, content_type, content_length, body)

    def health(self) -> dict:
        return self.server.health()

    def pump(self, server: NotesServer) -> None:
        """One explicit pipeline pass: no worker thread exists in this mode."""
        server.pipeline.run_once()


class SocketTransport:
    """A real POST to uvicorn on 127.0.0.1 — the wire the device actually uses."""

    name = "socket"

    def __init__(self, port: int) -> None:
        self.base = f"http://127.0.0.1:{port}"

    def _call(self, method: str, path: str, body: bytes | None = None,
              headers: dict[str, str] | None = None) -> tuple[int, dict]:
        request = urllib.request.Request(self.base + path, data=body, headers=headers or {},
                                         method=method)
        try:
            with urllib.request.urlopen(request, timeout=30.0) as response:
                return response.status, json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:  # 4xx/5xx are answers, not failures
            return error.code, json.loads(error.read() or b"{}")

    def post(self, headers: dict[str, str], content_type: str, content_length: int | None,
             body: bytes) -> tuple[int, dict]:
        wire = {"Content-Type": content_type, **headers}
        if content_length is not None:
            wire["Content-Length"] = str(content_length)
        return self._call("POST", NOTE_PATH, body, wire)

    def health(self) -> dict:
        status, payload = self._call("GET", HEALTH_PATH)
        assert status == 200, (status, payload)
        return payload

    def pump(self, server: NotesServer) -> None:
        server.notify()  # the worker thread exists here; never drive a second pass concurrently


def upload(transport, audio: bytes, codec: str, *, parts: list[Part] | None = None,
           content_type: str | None = None, content_length: int | str | None = "auto",
           **sign) -> tuple[int, dict]:
    """One signed multipart upload, with overrides for the failure cases."""
    parts = parts if parts is not None else upload_parts(audio, codec, **{
        k: v for k, v in sign.items() if k in {"device", "recorded_at", "duration", "idem"}
    })
    headers = signed_headers(audio, **{k: v for k, v in sign.items()
                                       if k in {"device", "recorded_at", "duration", "idem", "key", "ts", "mac"}})
    body = multipart(parts)
    if content_length == "auto":
        content_length = len(body)
    content_type = content_type or f"multipart/form-data; boundary={BOUNDARY}"
    return transport.post(headers, content_type, content_length, body)


# -- doubles and clock ------------------------------------------------------------------


@dataclass
class Doubles:
    """The two external services CI cannot run, and nothing else.

    ``transcribe`` verifies it is handed the exact bytes the endpoint staged, so
    a broken staging path cannot be masked by the double. ``summarize`` checks
    the transcript it was given is the STT output verbatim (§5.6).
    """

    transcript: str = TRANSCRIPT
    title: str = TITLE
    summary: str = SUMMARY
    key_points: tuple[str, ...] = KEY_POINTS
    audio_bytes: bytes = b""
    codec: str = "wav"
    stt_error: BaseException | None = None
    stt_calls: int = 0
    summarize_calls: int = 0
    seen: list[str] = field(default_factory=list)

    def transcribe(self, audio_path: Path, meta) -> str:
        self.stt_calls += 1
        assert Path(audio_path).read_bytes() == self.audio_bytes, "staged audio is not what was uploaded"
        assert meta.codec == self.codec, (meta.codec, self.codec)
        if self.stt_error is not None:
            raise self.stt_error
        return self.transcript

    def summarize(self, transcript: str) -> dict:
        self.summarize_calls += 1
        assert transcript == self.transcript, "the transcript was rewritten before the model saw it"
        return {"title": self.title, "summary": self.summary, "key_points": list(self.key_points)}


class Clock:
    """Injectable pipeline clock: the retry schedule is asserted, not waited on."""

    def __init__(self, now: float = 1_800_000_000.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


# -- fixtures ---------------------------------------------------------------------------


@pytest.fixture
def doubles() -> Doubles:
    return Doubles()


@pytest.fixture
def clock() -> Clock:
    return Clock()


@pytest.fixture
def server(tmp_path, doubles, clock) -> NotesServer:
    """A live server over an enrolled device and a temporary vault.

    ``obsidian_dir`` is the configured folder itself (§6 default ends in
    ``voice notes``), so the assertions can prove the note landed *under*
    ``voice notes``.
    """
    store = DeviceStore(tmp_path / "plugin-data")
    store.enroll(DEVICE, KEY, name="EPaper")
    store.confirm(DEVICE)
    server = NotesServer(
        store, tmp_path / "notes", host="127.0.0.1", port=0,
        obsidian_dir=tmp_path / "vault" / "voice notes",
        transcribe=doubles.transcribe, summarize=doubles.summarize, clock=clock,
    )
    try:
        yield server
    finally:
        if server.bound_port is not None:
            server.stop()


@pytest.fixture(params=["direct", "socket"])
def transport(request, server) -> DirectTransport | SocketTransport:
    if request.param == "direct":
        yield DirectTransport(server)
        return
    pytest.importorskip("uvicorn", reason="the socket transport needs uvicorn (ships with Hermes)")
    pytest.importorskip("fastapi", reason="the socket transport needs fastapi (ships with Hermes)")
    server.start()
    yield SocketTransport(server.bound_port)


@pytest.fixture(params=["wav", "opus"])
def sample(request, doubles) -> tuple[bytes, str]:
    path = SAMPLE[request.param]
    assert path.is_file(), f"fixture missing: {path}"
    audio = path.read_bytes()
    doubles.audio_bytes = audio
    doubles.codec = request.param
    return audio, request.param


def wait_for_job(server: NotesServer, transport, job_id: str, *, timeout: float = 30.0) -> Path:
    """Drive/wait for the pipeline until the job is closed (done or failed)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if (server.done / job_id).is_dir() or (server.failed / job_id).is_dir():
            break
        transport.pump(server)
        time.sleep(0.05)
    else:
        pytest.fail(f"pipeline never closed {job_id}: counts={server.counts()} last_error={server.last_error}")
    assert not (server.failed / job_id).is_dir(), f"job was quarantined: {(server.failed / job_id / '.error').read_text()}"
    entry = server.done / job_id
    assert entry.is_dir(), server.counts()
    return entry


def notes_in(vault: Path) -> list[Path]:
    return sorted(vault.glob("*.md"))


# -- the happy path: upload -> STT -> summary -> note -----------------------------------


def test_upload_becomes_exactly_one_note(server, transport, sample, tmp_path, doubles):
    """§8: WAV and Opus, over the real endpoint, into ``voice notes/`` once."""
    audio, codec = sample
    vault = tmp_path / "vault" / "voice notes"

    status, payload = upload(transport, audio, codec)
    assert status == 202, payload
    assert payload == {"status": "accepted", "job_id": JOB_ID}

    # 202 is the durable-staging answer, before any model call (§3.1). The
    # direct transport has no worker, so this is observable deterministically.
    if transport.name == "direct":
        assert doubles.stt_calls == 0, "the upload waited for STT"

    entry = wait_for_job(server, transport, JOB_ID)
    assert sorted(p.name for p in entry.iterdir()) == ["note.json", f"recording.{codec}"]
    staged = json.loads((entry / "note.json").read_text())
    assert staged["job_id"] == JOB_ID
    assert staged["device_id"] == DEVICE
    assert staged["recorded_at_utc"] == RECORDED_AT_UTC
    assert staged["audio_sha256"] == hashlib.sha256(audio).hexdigest()
    assert staged["codec"] == codec and staged["supplied_key"] == IDEMPOTENCY_KEY

    # -- the note: one file, the contract's name and folder (§5.4, §5.5)
    notes = notes_in(vault)
    assert [p.name for p in notes] == [expected_note_name(JOB_ID)], [p.name for p in notes]
    note = notes[0]
    assert note.parent.name == "voice notes" and note.parent.parent == tmp_path / "vault"

    # -- the markdown: fixed frontmatter order (§5.6) and both texts
    text = note.read_text(encoding="utf-8")
    lines = text.splitlines()
    assert lines[0] == "---"
    audio_name = f"{note.stem}.{codec}"
    assert lines[1:8] == [
        f"idempotency_key: {JOB_ID}",
        f"device_id: {DEVICE}",
        f"recorded_at: {RECORDED_AT_UTC}",
        f"duration_seconds: {DURATION}",
        f"codec: {codec}",
        f"audio_sha256: {hashlib.sha256(audio).hexdigest()}",
        f"audio: _audio/{audio_name}",
    ]
    assert lines[8] == f"title: {TITLE}"
    assert lines[9] == "transcript: |"
    assert f"  {TRANSCRIPT}" in lines[10:], "the transcript is not in the frontmatter"
    assert "summary: |" in lines and f"  {SUMMARY}" in lines
    assert "key_points:" in lines and f"  - {KEY_POINTS[0]}" in lines
    assert "tags: [voice-note]" in lines and lines[-1] == TRANSCRIPT

    front, body = text.split("\n---\n", 1)
    assert TRANSCRIPT in front and SUMMARY in front
    assert body.lstrip("\n").startswith(f"# {TITLE}\n")
    for heading, content in (("# " + TITLE, None), ("## Summary", SUMMARY),
                             ("## Key points", f"- {KEY_POINTS[0]}"), ("## Transcript", TRANSCRIPT)):
        assert heading in body, heading
        if content is not None:
            assert content in body, content
    order = [body.index("## Summary"), body.index("## Key points"), body.index("## Transcript")]
    assert order == sorted(order), "body sections are out of contract order"
    assert body.rstrip().endswith(TRANSCRIPT), "the transcript is not verbatim at the end of the body"

    # -- the audio copy beside the notes, byte-identical (§5.4)
    copied = vault / "_audio" / audio_name
    assert copied.read_bytes() == audio

    # -- health and counts
    health = transport.health()
    assert health["status"] == "ok" and health["last_error"] is None
    assert (health["inbox"], health["done"], health["failed"]) == (0, 1, 0), health
    if transport.name == "socket":
        assert health["port"] == server.bound_port
    assert doubles.summarize_calls == 1


def test_the_same_upload_again_is_a_duplicate_not_a_second_note(server, transport, sample, tmp_path):
    """§5.3: same key, same bytes -> 200 duplicate, one note, one done entry."""
    audio, codec = sample
    vault = tmp_path / "vault" / "voice notes"
    assert upload(transport, audio, codec)[0] == 202
    wait_for_job(server, transport, JOB_ID)
    first = notes_in(vault)[0].read_text(encoding="utf-8")

    status, payload = upload(transport, audio, codec)
    assert status == 200, payload
    assert payload == {"status": "duplicate", "job_id": JOB_ID}

    # An extra pipeline pass on a closed job writes nothing new.
    transport.pump(server)
    time.sleep(0.1)
    transport.pump(server)
    assert [p.name for p in notes_in(vault)] == [expected_note_name(JOB_ID)]
    assert notes_in(vault)[0].read_text(encoding="utf-8") == first
    assert server.counts() == {"inbox": 0, "done": 1, "failed": 0}


def test_the_same_key_with_different_audio_is_a_conflict(server, transport, sample, tmp_path):
    """§5.1/§5.3: a supplied key excludes the content, so different bytes are 409."""
    audio, codec = sample
    assert upload(transport, audio, codec)[0] == 202
    wait_for_job(server, transport, JOB_ID)

    changed = audio + b"\x00\x00"  # still a valid container, different digest
    status, payload = upload(transport, changed, codec)
    assert status == 409, payload
    error = payload["error"]
    assert error["code"] == "idempotency_conflict" and error["retryable"] is False
    assert server.counts() == {"inbox": 0, "done": 1, "failed": 0}


def test_one_recording_under_two_job_ids_still_makes_one_note(server, transport, sample, tmp_path):
    """§5.3: no supplied key -> a second job id, but the frontmatter dedupe holds."""
    audio, codec = sample
    second_job = job_id_for(key=None, audio=audio)
    assert second_job != JOB_ID

    assert upload(transport, audio, codec)[0] == 202
    wait_for_job(server, transport, JOB_ID)

    status, payload = upload(transport, audio, codec, parts=upload_parts(audio, codec, idem=None), idem=None)
    assert status == 202 and payload == {"status": "accepted", "job_id": second_job}, payload
    wait_for_job(server, transport, second_job)

    assert [p.name for p in notes_in(tmp_path / "vault" / "voice notes")] == [expected_note_name(JOB_ID)]
    assert server.counts() == {"inbox": 0, "done": 2, "failed": 0}


# -- validation and authorization failures (§3.2, §4, §8) ------------------------------
#
# These drive the direct transport: the failures are answered before any model
# call, and the same code answers them over the socket (pinned in
# tests/test_notes_ingest.py).


@dataclass
class Case:
    id: str
    status: int
    code: str
    bad_field: str | None = None
    content_type: str | None = None
    content_length: int | str | None = "auto"
    parts: list[Part] | None = None
    sign: dict = field(default_factory=dict)


def wire_cases(audio: bytes, codec: str) -> list[Case]:
    return [
        Case("missing device_id", 400, "bad_request", "device_id",
             parts=upload_parts(audio, codec, drop=("device_id",))),
        Case("missing recorded_at", 400, "bad_request", "recorded_at",
             parts=upload_parts(audio, codec, drop=("recorded_at",))),
        Case("missing duration_seconds", 400, "bad_request", "duration_seconds",
             parts=upload_parts(audio, codec, drop=("duration_seconds",))),
        Case("audio sent twice", 400, "bad_request", "audio",
             parts=upload_parts(audio, codec, audio_twice=True)),
        Case("audio as a text part", 400, "bad_request", "audio",
             parts=upload_parts(audio, codec, audio_as_text=True)),
        Case("empty audio", 400, "bad_request", "audio",
             parts=upload_parts(b"", codec)),
        Case("not multipart", 400, "bad_request", content_type="application/octet-stream"),
        Case("bad device id", 422, "invalid_field", "device_id", sign={"device": "no spaces allowed"}),
        Case("naive recorded_at", 422, "invalid_field", "recorded_at",
             sign={"recorded_at": "2026-11-20T14:15:04"}),
        Case("recorded_at in the future", 422, "invalid_field", "recorded_at",
             sign={"recorded_at": "2030-01-01T00:00:00Z"}),
        Case("duration 0", 422, "invalid_field", "duration_seconds", sign={"duration": "0"}),
        Case("duration 86401", 422, "invalid_field", "duration_seconds", sign={"duration": "86401"}),
        Case("duration not an integer", 422, "invalid_field", "duration_seconds", sign={"duration": "4.5"}),
        Case("idempotency key with a space", 422, "invalid_field", "idempotency_key",
             sign={"idem": "not a key"}),
        Case("unknown device", 401, "auth", sign={"device": OTHER_DEVICE}),
        Case("wrong MAC", 401, "auth", sign={"mac": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}),
        Case("timestamp outside the window", 401, "auth",
             sign={"ts": str(int(time.time()) - 301)}),
        Case("checksum mismatch", 409, "checksum_mismatch",
             parts=upload_parts(audio, codec, checksum="0" * 64)),
        Case("unsupported codec", 415, "unsupported_codec", "audio",
             parts=upload_parts(b"ID3\x04\x00\x00\x00\x00\x00\x00", codec)),
        Case("request over the cap", 413, "too_large", content_length=MAX_REQUEST_BYTES + 1),
    ]


@pytest.mark.parametrize("codec", ["wav", "opus"])
def test_every_rejection_answers_the_contract_error(server, doubles, codec):
    audio = SAMPLE[codec].read_bytes()
    doubles.audio_bytes = audio
    transport = DirectTransport(server)
    vault = server.pipeline.obsidian_dir

    for case in wire_cases(audio, codec):
        status, payload = upload(transport, audio, codec, parts=case.parts,
                                 content_type=case.content_type,
                                 content_length=case.content_length, **case.sign)
        assert (status, payload.get("error", {}).get("code")) == (case.status, case.code), (case.id, payload)
        error = payload["error"]
        assert set(payload) == {"error"}, case.id
        assert error["retryable"] is False, case.id
        assert isinstance(error["message"], str) and error["message"], case.id
        if case.bad_field is not None:
            assert error.get("field") == case.bad_field, (case.id, error)
        assert server.counts() == {"inbox": 0, "done": 0, "failed": 0}, case.id
        assert not notes_in(vault), case.id
        assert doubles.stt_calls == 0 and doubles.summarize_calls == 0, case.id


def test_a_paused_endpoint_answers_503_retryable(server):
    """§3.2/§4 step 9: maintenance is retryable, and nothing is stored."""
    audio = SAMPLE["wav"].read_bytes()
    server.set_paused(True)
    status, payload = upload(DirectTransport(server), audio, "wav")
    assert status == 503 and payload["error"]["code"] == "unavailable"
    assert payload["error"]["retryable"] is True
    assert server.counts() == {"inbox": 0, "done": 0, "failed": 0}


# -- a retryable processing failure: backoff, attempts, recovery -----------------------


def test_a_retryable_stt_failure_backs_off_then_recovers(server, doubles, clock, tmp_path):
    """§5.8/§8: provider down -> count the attempt, wait 30 s, retry, then write."""
    audio = SAMPLE["wav"].read_bytes()
    doubles.audio_bytes = audio
    transport = DirectTransport(server)
    vault = server.pipeline.obsidian_dir
    entry = server.inbox / JOB_ID

    doubles.stt_error = ConnectionError("model provider timed out")
    assert upload(transport, audio, "wav")[0] == 202

    server.pipeline.run_once()
    assert doubles.stt_calls == 1
    assert entry.is_dir(), "a retryable failure must keep the job in inbox/"
    bookkeeping = json.loads((entry / "note.json").read_text())
    assert bookkeeping["attempts"] == 1
    assert bookkeeping["last_error"].startswith("connect: model provider timed out")
    assert not (entry / ".error").exists(), "a retryable failure must not quarantine"
    assert not notes_in(vault), "a failed attempt wrote a note"

    # Not due yet: a second pass without advancing the clock must not call STT again.
    server.pipeline.run_once()
    assert doubles.stt_calls == 1, "the backoff was ignored"

    clock.advance(BACKOFF_S[0])  # 30 s: the first retry is due
    server.pipeline.run_once()
    assert doubles.stt_calls == 2
    assert json.loads((entry / "note.json").read_text())["attempts"] == 2
    assert not notes_in(vault)

    doubles.stt_error = None
    clock.advance(BACKOFF_S[1])  # 60 s: the second retry is due and succeeds
    server.pipeline.run_once()
    assert server.done.joinpath(JOB_ID).is_dir()
    assert [p.name for p in notes_in(vault)] == [expected_note_name(JOB_ID)]
    assert TRANSCRIPT in notes_in(vault)[0].read_text(encoding="utf-8")
    assert doubles.summarize_calls == 1
    assert server.counts() == {"inbox": 0, "done": 1, "failed": 0}


def test_eight_failures_quarantine_the_job(server, doubles, clock):
    """§5.8/§8: the attempt cap quarantines into failed/ with .error, and the
    schedule is the contract's 30 s … 1920 s ladder."""
    assert tuple(notes_pipeline.RETRY_BACKOFF_S) == BACKOFF_S
    assert notes_pipeline.MAX_ATTEMPTS == MAX_ATTEMPTS

    audio = SAMPLE["opus"].read_bytes()
    doubles.audio_bytes = audio
    doubles.codec = "opus"
    transport = DirectTransport(server)
    entry_id = job_id_for()

    doubles.stt_error = ConnectionError("model provider unreachable")
    assert upload(transport, audio, "opus")[0] == 202

    for attempt in range(1, MAX_ATTEMPTS):  # 1..7: counted, still queued
        server.pipeline.run_once()
        assert (server.inbox / entry_id).is_dir(), attempt
        assert json.loads((server.inbox / entry_id / "note.json").read_text())["attempts"] == attempt
        clock.advance(BACKOFF_S[attempt - 1])

    server.pipeline.run_once()  # the eighth failure is the cap
    assert not (server.inbox / entry_id).exists()
    failed = server.failed / entry_id
    assert failed.is_dir()
    error = json.loads((failed / ".error").read_text())
    assert error["code"] == "attempts_exhausted"
    assert error["step"] == "stt"
    assert error["attempts"] == MAX_ATTEMPTS
    assert "model provider unreachable" in error["message"]
    assert doubles.stt_calls == MAX_ATTEMPTS
    assert not notes_in(server.pipeline.obsidian_dir)
    assert server.counts() == {"inbox": 0, "done": 0, "failed": 1}
    assert server.health()["failed"] == 1


def test_a_terminal_stt_error_quarantines_immediately(server, doubles):
    """§5.8: a 4xx from the provider is terminal — no retries, no note."""
    audio = SAMPLE["wav"].read_bytes()
    doubles.audio_bytes = audio
    doubles.stt_error = notes_pipeline.PipelineError(
        "provider rejected the credentials", step="stt", code="unauthenticated", retryable=False)
    assert upload(DirectTransport(server), audio, "wav")[0] == 202

    server.pipeline.run_once()
    failed = server.failed / JOB_ID
    assert failed.is_dir() and not notes_in(server.pipeline.obsidian_dir)
    error = json.loads((failed / ".error").read_text())
    assert error["code"] == "unauthenticated" and error["step"] == "stt" and error["attempts"] == 1
    assert doubles.stt_calls == 1
