"""The voice-note upload contract (docs/voice-note-ingestion.md), enforced here.

These tests drive ``NotesServer.handle_upload`` directly plus the ASGI app
through ``TestClient``, with a fake device store: no sockets, no Hermes, no
model. What they pin is what the firmware is written against — the multipart
parts, the status codes and error envelope of §3, the validation order of §4,
the idempotency matrix of §5.3, and that nothing reaches the inbox unless every
check passed.
"""

from __future__ import annotations

import errno
import hashlib
import io
import json
import os
import threading
import time
import wave
from datetime import datetime, timedelta, timezone

import pytest

from hermes_gadget_plugin import notes_ingest, protocol
from hermes_gadget_plugin.notes_ingest import (
    MAX_NOTE_BYTES,
    MAX_REQUEST_BYTES,
    NotesServer,
    create_app,
)

pytest.importorskip("fastapi")
pytest.importorskip("httpx")

from fastapi.testclient import TestClient  # noqa: E402

KEY = bytes(range(32))
DEVICE = protocol.device_id_for_key(KEY)
BOUNDARY = "----hermesGadgetBoundary7d01"
RECORDED_AT = "2026-11-20T08:15:04-06:00"


# -- fixtures and wire helpers ---------------------------------------------------------


class FakeStore:
    """The slice of ``store.DeviceStore`` the endpoint uses."""

    def __init__(self, keys=None):
        self.keys = dict(keys if keys is not None else {DEVICE: KEY})

    def key_for(self, device_id):
        return self.keys.get(device_id)

    def devices(self):
        return {did: {"name": "EPaper", "firmware": "0.1.0"} for did in self.keys}


@pytest.fixture
def server(tmp_path):
    return NotesServer(FakeStore(), tmp_path / "notes", host="127.0.0.1", port=0,
                       obsidian_dir=tmp_path / "vault",
                       transcribe=lambda audio, meta: "irrelevant here",
                       summarize=lambda text: "irrelevant here")


def wav_bytes(rate=24000, channels=1, sample_width=2, frames=240) -> bytes:
    """A real (short) PCM WAV the codec sniff can accept."""
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(sample_width)
        handle.setframerate(rate)
        handle.writeframes(b"\x00\x01" * (frames * channels))
    return buffer.getvalue()


def float_wav_bytes() -> bytes:
    """A WAV with format tag 3 (IEEE float), which the contract rejects."""
    data = b"\x00\x00\x00\x00" * 64
    fmt = (3).to_bytes(2, "little") + bytes([1]) + (0).to_bytes(2, "little") \
        + (24000).to_bytes(4, "little") + (96000).to_bytes(4, "little") \
        + (4).to_bytes(2, "little") + (32).to_bytes(2, "little")
    body = b"fmt " + len(fmt).to_bytes(4, "little") + fmt
    body += b"data" + len(data).to_bytes(4, "little") + data
    return b"RIFF" + (4 + len(body)).to_bytes(4, "little") + b"WAVE" + body


def opus_bytes(channels=1) -> bytes:
    """A minimal Ogg page whose first packet is an ``OpusHead``."""
    head = (b"OpusHead" + bytes([1, channels]) + (312).to_bytes(2, "little")
            + (24000).to_bytes(4, "little") + (0).to_bytes(2, "little") + bytes([0]))
    return (b"OggS" + b"\x00\x02" + (0).to_bytes(8, "little") + (1).to_bytes(4, "little")
            + (0).to_bytes(4, "little") + b"\x00\x00\x00\x00" + bytes([1, len(head)]) + head)


WAV = wav_bytes()


def recent_recorded_at(minutes_ago=5) -> str:
    """An ISO-8601 stamp with a -06:00 offset that is always safely in the past."""
    moment = datetime.now(timezone.utc) - timedelta(minutes=minutes_ago)
    return moment.astimezone(timezone(timedelta(hours=-6))).replace(microsecond=0).isoformat()


def fields(**overrides) -> dict:
    base = {"device_id": DEVICE, "recorded_at": recent_recorded_at(), "duration_seconds": "8"}
    base.update(overrides)
    return {key: value for key, value in base.items() if value is not None}


def part(name, value: bytes | str, *, filename=None, content_type=None) -> bytes:
    disposition = f'Content-Disposition: form-data; name="{name}"'
    if filename is not None:
        disposition += f'; filename="{filename}"'
    head = f"--{BOUNDARY}\r\n{disposition}\r\n"
    if content_type:
        head += f"Content-Type: {content_type}\r\n"
    head += "\r\n"
    payload = value.encode() if isinstance(value, str) else value
    return head.encode() + payload + b"\r\n"


def multipart(body_fields=None, audio=WAV, *, audio_name="note.wav",
              audio_type="audio/wav", audio_as_text=False, extra=(), eat_crlf=False) -> bytes:
    chunks = [part(name, value) for name, value in (body_fields or fields()).items()]
    if audio is not None:
        chunks.append(part("audio", audio, filename=None if audio_as_text else audio_name,
                           content_type=audio_type))
    chunks.extend(extra)
    chunks.append(f"--{BOUNDARY}--\r\n".encode())
    return b"".join(chunks)


def mac_for(body_fields: dict, audio: bytes = WAV, *, key=KEY, ts=None) -> str:
    ts = str(int(time.time())) if ts is None else str(ts)
    return protocol.note_mac(key, body_fields.get("device_id", DEVICE), ts,
                             body_fields["recorded_at"], body_fields["duration_seconds"],
                             hashlib.sha256(audio).hexdigest(),
                             body_fields.get("idempotency_key"))


def signed(body_fields: dict, audio: bytes = WAV, *, key=KEY, ts=None, mac=None) -> dict:
    ts = str(int(time.time())) if ts is None else str(ts)
    headers = {"x-gadget-ts": ts,
               "x-gadget-mac": mac if mac is not None else mac_for(body_fields, audio, key=key, ts=ts)}
    return headers


_DEFAULT_CONTENT_TYPE = object()


def upload(server, body_fields=None, audio=WAV, *, headers=None, body=None,
           content_type=_DEFAULT_CONTENT_TYPE, content_length: int | str | None = "auto", **kwargs):
    body_fields = fields() if body_fields is None else body_fields
    body = multipart(body_fields, audio, **kwargs) if body is None else body
    if headers is None:
        try:  # malformed-request tests never reach auth; sign what can be signed
            headers = signed(body_fields, audio)
        except (KeyError, TypeError):
            headers = {}
    if content_type is _DEFAULT_CONTENT_TYPE:
        content_type = f"multipart/form-data; boundary={BOUNDARY}"
    length = len(body) if content_length == "auto" else content_length
    return server.handle_upload(headers, content_type, length, body)


def error_of(payload):
    return payload["error"]


# -- acceptance ------------------------------------------------------------------------


def test_valid_wav_upload_is_staged_exactly_once(server):
    body_fields = fields()
    status, payload = upload(server, body_fields)
    # The job id derives from the normalized stamp, not the raw one.
    normalized = notes_ingest.normalize_recorded_at(body_fields["recorded_at"])
    expected_job = hashlib.sha256(
        f"{DEVICE}|{normalized}|{hashlib.sha256(WAV).hexdigest()}".encode()).hexdigest()
    assert (status, payload) == (202, {"status": "accepted", "job_id": expected_job})
    assert len(expected_job) == 64

    entry = server.inbox / expected_job
    assert (entry / "recording.wav").read_bytes() == WAV
    note = json.loads((entry / "note.json").read_text())
    assert note["job_id"] == expected_job
    assert note["device_id"] == DEVICE
    assert note["recorded_at"] == body_fields["recorded_at"]  # raw, exactly as sent
    assert note["recorded_at_utc"] == normalized
    assert note["duration_seconds"] == 8
    assert note["audio_sha256"] == hashlib.sha256(WAV).hexdigest()
    assert note["codec"] == "wav"
    assert note["supplied_key"] is None
    assert note["attempts"] == 0 and note["last_error"] is None
    assert isinstance(note["accepted_unix"], int)
    # nothing half-written is left behind for the pipeline to find
    assert not any(p.name.startswith(".") for p in entry.iterdir())
    assert not list((server.notes_dir / ".staging").glob("*"))


def test_valid_opus_upload_is_staged(server):
    opus = opus_bytes()
    status, payload = upload(server, fields(), opus, audio_name="note.opus", audio_type="audio/ogg")
    assert status == 202 and payload["status"] == "accepted"
    entry = server.inbox / payload["job_id"]
    assert (entry / "recording.opus").read_bytes() == opus
    assert json.loads((entry / "note.json").read_text())["codec"] == "opus"


def test_supplied_key_derives_the_job_id_and_is_kept(server):
    body_fields = fields(idempotency_key="dev-key-01")
    status, payload = upload(server, body_fields)
    assert status == 202
    assert payload["job_id"] == hashlib.sha256(f"{DEVICE}|dev-key-01".encode()).hexdigest()
    note = json.loads((server.inbox / payload["job_id"] / "note.json").read_text())
    assert note["supplied_key"] == "dev-key-01"


def test_unknown_parts_are_ignored(server):
    status, _ = upload(server, fields(), extra=[part("firmware", "0.4.2"), part("battery", "88")])
    assert status == 202


# -- idempotency (§5.3) ----------------------------------------------------------------


def test_same_key_twice_is_202_then_200_duplicate(server):
    body_fields = fields(idempotency_key="dev-key-01")
    first = upload(server, body_fields)
    assert first[0] == 202
    stored = server.inbox / first[1]["job_id"] / "recording.wav"
    before = stored.stat().st_mtime_ns
    second = upload(server, body_fields)
    assert second == (200, {"status": "duplicate", "job_id": first[1]["job_id"]})
    assert stored.stat().st_mtime_ns == before  # not rewritten
    assert server.counts()["inbox"] == 1


def test_derived_key_makes_a_retry_a_duplicate(server):
    body_fields = fields()  # no idempotency_key: the recording itself is the key
    assert upload(server, body_fields)[0] == 202
    assert upload(server, body_fields)[0] == 200


def test_a_note_the_pipeline_already_closed_still_answers_duplicate(server):
    body_fields = fields(idempotency_key="dev-key-02")
    status, payload = upload(server, body_fields)
    server.done.mkdir(parents=True, exist_ok=True)  # the pipeline moved it on
    (server.inbox / payload["job_id"]).rename(server.done / payload["job_id"])
    assert upload(server, body_fields) == (200, {"status": "duplicate", "job_id": payload["job_id"]})


def test_reusing_a_key_with_different_audio_conflicts(server):
    body_fields = fields(idempotency_key="dev-key-03")
    first = upload(server, body_fields)
    assert first[0] == 202
    other = wav_bytes(frames=120)
    status, payload = upload(server, body_fields, other)
    assert status == 409
    assert error_of(payload)["code"] == "idempotency_conflict"
    assert error_of(payload)["retryable"] is False
    assert (server.inbox / first[1]["job_id"] / "recording.wav").read_bytes() == WAV


def test_a_quarantined_job_is_reopened_and_requeued(server):
    body_fields = fields(idempotency_key="dev-key-04")
    status, payload = upload(server, body_fields)
    job_id = payload["job_id"]
    server.failed.mkdir(parents=True, exist_ok=True)
    entry = server.inbox / job_id
    note = json.loads((entry / "note.json").read_text())
    note["attempts"] = 3
    note["last_error"] = "stt: connection reset"
    (entry / "note.json").write_text(json.dumps(note))
    (server.inbox / job_id).rename(server.failed / job_id)
    (server.failed / job_id / ".error").write_text(
        json.dumps({"code": "stt_unavailable", "step": "stt", "message": "down", "attempts": 3}))

    reopened = upload(server, body_fields)
    assert reopened == (202, {"status": "accepted", "job_id": job_id, "requeued": True})
    assert (server.inbox / job_id).is_dir() and not (server.failed / job_id).exists()
    assert not (server.inbox / job_id / ".error").exists()
    reset = json.loads((server.inbox / job_id / "note.json").read_text())
    assert reset["attempts"] == 0 and reset["last_error"] is None


def test_concurrent_identical_uploads_stage_one_entry(server):
    body_fields = fields(idempotency_key="dev-key-05")
    results = []

    def worker():
        results.append(upload(server, body_fields)[0])

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert sorted(results) == [200, 200, 200, 202]
    assert server.counts()["inbox"] == 1


# -- parse failures are 400 (§2, §4 step 4) -------------------------------------------


@pytest.mark.parametrize("missing", ["audio", "device_id", "recorded_at", "duration_seconds"])
def test_a_missing_required_part_is_400_with_the_field(server, missing):
    body_fields = fields()
    if missing == "audio":
        status, payload = upload(server, body_fields, audio=None)
    else:
        status, payload = upload(server, fields(**{missing: None}))
    assert status == 400
    assert error_of(payload)["code"] == "bad_request"
    assert error_of(payload)["field"] == missing
    assert error_of(payload)["retryable"] is False
    assert not server.inbox.exists() or server.counts()["inbox"] == 0


def test_audio_must_be_a_non_empty_file_part(server):
    status, payload = upload(server, audio_as_text=True)
    assert (status, error_of(payload)["field"]) == (400, "audio")
    status, payload = upload(server, audio=b"")
    assert status == 400 and error_of(payload)["field"] == "audio"


def test_a_repeated_part_is_400(server):
    repeated = multipart(extra=[part("device_id", DEVICE)])
    status, payload = upload(server, body=repeated)
    assert status == 400
    assert error_of(payload)["code"] == "bad_request"
    assert error_of(payload)["field"] == "device_id"


def test_metadata_parts_must_not_be_file_parts(server):
    """A ``device_id`` (or any metadata part) sent as a file part is a 400."""
    body_fields = {"device_id": DEVICE, "recorded_at": recent_recorded_at()}
    body = multipart(body_fields, extra=[part("duration_seconds", "8", filename="d.txt")])
    status, payload = upload(server, body=body)
    assert status == 400
    assert error_of(payload)["field"] == "duration_seconds"


def test_wrong_or_missing_content_type_is_400(server):
    body = multipart()
    assert upload(server, body=body, content_type="application/octet-stream")[0] == 400
    assert upload(server, body=body, content_type=None)[0] == 400
    assert upload(server, body=body, content_type="multipart/form-data")[0] == 400  # no boundary


def test_a_body_that_is_not_multipart_is_400(server):
    status, payload = upload(server, body=WAV)
    assert status == 400 and error_of(payload)["code"] == "bad_request"


# -- field formats are 422 (§2, §4 step 5) --------------------------------------------


@pytest.mark.parametrize("recorded_at", ["2026-11-20T08:15:04", "2026-11-20 08:15:04-06:00",
                                         "yesterday", "2026-13-40T08:15:04-06:00"])
def test_a_bad_recorded_at_is_422(server, recorded_at):
    status, payload = upload(server, fields(recorded_at=recorded_at))
    assert status == 422
    assert error_of(payload)["code"] == "invalid_field"
    assert error_of(payload)["field"] == "recorded_at"


def test_a_future_recorded_at_beyond_the_window_is_422(server):
    future = (datetime.now(timezone.utc) + timedelta(minutes=10)).replace(microsecond=0).isoformat()
    status, payload = upload(server, fields(recorded_at=future))
    assert status == 422 and error_of(payload)["field"] == "recorded_at"


@pytest.mark.parametrize("duration", ["0", "86401", "8.5", " 8", "eight", "", "-1"])
def test_a_bad_duration_is_422(server, duration):
    status, payload = upload(server, fields(duration_seconds=duration))
    assert status == 422 and error_of(payload)["field"] == "duration_seconds"


@pytest.mark.parametrize("device_id", ["has space", "-leading", "x" * 65, "héllo", "*star", ""])
def test_a_bad_device_id_is_422(server, device_id):
    status, payload = upload(server, fields(device_id=device_id))
    assert status == 422 and error_of(payload)["field"] == "device_id"


def test_a_bad_idempotency_key_is_422(server):
    status, payload = upload(server, fields(idempotency_key="bad key!"))
    assert status == 422 and error_of(payload)["field"] == "idempotency_key"


def test_a_bad_checksum_format_is_422(server):
    status, payload = upload(server, fields(checksum="ABC"))
    assert status == 422 and error_of(payload)["field"] == "checksum"


def test_validation_precedes_authentication(server):
    """§4: a malformed body is answered before any device key is consulted."""
    status, payload = upload(server, fields(duration_seconds="0"), headers={})
    assert status == 422 and error_of(payload)["field"] == "duration_seconds"
    status, payload = upload(server, fields(), headers={}, body=multipart(
        fields(device_id=None)))
    assert status == 400 and error_of(payload)["field"] == "device_id"


# -- codecs are 415 (§2.3) -------------------------------------------------------------


@pytest.mark.parametrize("audio", [
    b"ID3\x04\x00\x00\x00\x00\x00\x00" + b"\x00" * 200,  # MP3-ish
    wav_bytes(channels=2),  # stereo WAV
    wav_bytes(rate=96000),  # out of the rate band
    wav_bytes(sample_width=1),  # 8-bit WAV
    float_wav_bytes(),  # float WAV (format tag 3)
    opus_bytes(channels=2),  # stereo Opus
    b"OggS" + b"\x00" * 64,  # Ogg without an OpusHead
])
def test_unsupported_audio_is_415(server, audio):
    status, payload = upload(server, fields(), audio)
    assert status == 415
    assert error_of(payload)["code"] == "unsupported_codec"
    assert error_of(payload)["field"] == "audio"
    assert server.counts()["inbox"] == 0


# -- authentication is 401 (§2.1) ------------------------------------------------------


def test_unknown_device_is_401(server):
    server.store = FakeStore(keys={})
    status, payload = upload(server, fields())
    assert status == 401 and error_of(payload)["code"] == "auth"


def test_a_wrong_or_absent_mac_is_401(server):
    body_fields = fields()
    status, payload = upload(server, body_fields, headers={})
    assert status == 401 and error_of(payload)["code"] == "auth"
    forged = signed(body_fields, key=bytes(range(1, 33)))
    assert upload(server, body_fields, headers=forged)[0] == 401
    assert server.counts()["inbox"] == 0


def test_the_mac_covers_the_raw_recorded_at(server):
    """A MAC over a normalized stamp does not authorize the raw one (§2.2)."""
    body_fields = fields(recorded_at=RECORDED_AT)
    normalized = notes_ingest.normalize_recorded_at(RECORDED_AT)
    ts = str(int(time.time()))
    mac = protocol.note_mac(KEY, DEVICE, ts, normalized, body_fields["duration_seconds"],
                            hashlib.sha256(WAV).hexdigest(), None)
    assert upload(server, body_fields, headers={"x-gadget-ts": ts, "x-gadget-mac": mac})[0] == 401


def test_clock_skew_beyond_the_window_is_401(server):
    now = int(time.time())
    body_fields = fields()
    assert upload(server, body_fields, headers=signed(body_fields, ts=now - 301))[0] == 401
    assert upload(server, body_fields, headers=signed(body_fields, ts=now + 301))[0] == 401
    assert upload(server, body_fields, headers=signed(body_fields, ts=now - 299))[0] == 202


def test_a_non_numeric_timestamp_is_401(server):
    headers = signed(fields())
    headers["x-gadget-ts"] = "soon"
    assert upload(server, fields(), headers=headers)[0] == 401


def test_access_token_is_required_when_configured(tmp_path):
    server = NotesServer(FakeStore(), tmp_path / "notes", host="127.0.0.1", port=0,
                         obsidian_dir=tmp_path / "vault", access_token="letmein",
                         transcribe=lambda a, m: "x", summarize=lambda t: "y")
    body_fields = fields()
    assert upload(server, body_fields)[0] == 401
    wrong = signed(body_fields)
    wrong["x-gadget-token"] = "wrong"
    assert upload(server, body_fields, headers=wrong)[0] == 401
    right = signed(body_fields)
    right["x-gadget-token"] = "letmein"
    assert upload(server, body_fields, headers=right)[0] == 202


# -- checksum is 409 (§2) --------------------------------------------------------------


def test_a_checksum_that_does_not_match_the_audio_is_409(server):
    body_fields = fields(checksum=hashlib.sha256(wav_bytes(frames=99)).hexdigest())
    status, payload = upload(server, body_fields)
    assert status == 409
    assert error_of(payload)["code"] == "checksum_mismatch"
    assert error_of(payload)["retryable"] is False
    assert server.counts()["inbox"] == 0


def test_a_matching_checksum_is_accepted(server):
    body_fields = fields(checksum=hashlib.sha256(WAV).hexdigest())
    assert upload(server, body_fields)[0] == 202


# -- size caps are 413 (§2.4) ----------------------------------------------------------

def test_the_caps_are_the_contracts():
    # 2 GiB, the device's own per-note ceiling (kMaxNoteBytes): the two sides
    # carry the same number so a note the firmware accepted is never refused here.
    assert MAX_NOTE_BYTES == protocol.NOTE_MAX_BYTES == 2 * 1024 * 1024 * 1024
    assert MAX_REQUEST_BYTES == MAX_NOTE_BYTES + 64 * 1024
    # duration_seconds is 1..86400 (24 h), the firmware's kMaxDurationS.
    assert (protocol.NOTE_DURATION_MIN, protocol.NOTE_DURATION_MAX) == (1, 86400)


def test_an_oversize_content_length_is_413_before_the_body(server):
    status, payload = upload(server, body=b"", content_length=MAX_REQUEST_BYTES + 1)
    assert status == 413 and error_of(payload)["code"] == "too_large"


def test_an_oversize_audio_part_is_413(server, tmp_path):
    small = NotesServer(FakeStore(), tmp_path / "small", host="127.0.0.1", port=0,
                        obsidian_dir=tmp_path / "vault", max_bytes=1024,
                        transcribe=lambda a, m: "x", summarize=lambda t: "y")
    status, payload = upload(small, fields(), wav_bytes(frames=2000))
    assert status == 413
    assert error_of(payload)["field"] == "audio"
    assert small.counts()["inbox"] == 0


def test_an_oversize_body_without_a_length_is_413(server, monkeypatch):
    monkeypatch.setattr(notes_ingest, "MAX_REQUEST_BYTES", 1024)  # the real cap is 2 GiB + 64 KiB
    huge = wav_bytes(frames=1024)
    status, payload = upload(server, fields(), huge, body=multipart(fields(), huge),
                             content_length=None)
    assert status == 413 and error_of(payload)["code"] == "too_large"


# -- a note past the old 32 MiB / 15 min caps ingests unchanged (§2.4) -------------------


def a_long_note(mib=34):
    """A real WAV past the old 32 MiB cap; 24 kHz PCM16 mono is 48 KB/s, so 34 MiB
    is about 12 minutes and a 20-minute note is 57.6 MB."""
    return wav_bytes(frames=(mib * 1024 * 1024) // 2)


def sha256_of(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def test_a_twelve_hour_duration_is_accepted(server):
    # The device's own ceiling (kMaxDurationS): an hour-long note is no longer 422.
    status, payload = upload(server, fields(duration_seconds="43200"))
    assert status == 202 and payload["status"] == "accepted"


def test_a_note_over_the_old_32_mib_cap_is_accepted_whole(server):
    audio = a_long_note()
    assert len(audio) > 32 * 1024 * 1024
    body_fields = fields(duration_seconds="1200", idempotency_key="long-note-direct")
    status, payload = upload(server, body_fields, audio)
    assert status == 202 and payload["status"] == "accepted"
    entry = server.inbox / payload["job_id"]
    stored = entry / "recording.wav"
    assert stored.stat().st_size == len(audio)  # nothing truncated or dropped
    assert sha256_of(stored) == hashlib.sha256(audio).hexdigest()
    assert json.loads((entry / "note.json").read_text())["duration_seconds"] == 1200


def test_a_large_note_goes_through_the_http_route_unchanged(server):
    # The route spools the body to the staging area and parses an mmap of it, so a
    # note the old cap refused must arrive byte-identical and leave no spool behind.
    audio = a_long_note()
    body_fields = fields(duration_seconds="1200", idempotency_key="long-note-route")
    body = multipart(body_fields, audio)
    wire = signed(body_fields, audio)
    wire["content-type"] = f"multipart/form-data; boundary={BOUNDARY}"
    response = TestClient(create_app(server)).post("/notes/v1", content=body, headers=wire)
    assert response.status_code == 202, response.text
    entry = server.inbox / response.json()["job_id"]
    assert (entry / "recording.wav").stat().st_size == len(audio)
    assert sha256_of(entry / "recording.wav") == hashlib.sha256(audio).hexdigest()
    assert list(server.staging_dir.iterdir()) == []


# -- storage failures 507 / 503 (§3.2, §5.7) -------------------------------------------


def test_a_storage_failure_is_507_and_leaves_nothing(server, monkeypatch):
    def full(*args, **kwargs):
        raise OSError(errno.ENOSPC, "No space left on device")

    monkeypatch.setattr(os, "rename", full)
    status, payload = upload(server, fields())
    assert status == 507
    assert error_of(payload)["code"] == "storage"
    assert error_of(payload)["retryable"] is True
    assert server.counts()["inbox"] == 0
    assert not list((server.notes_dir / ".staging").glob("*"))  # staging cleaned up


def test_transient_staging_errors_are_retried_in_request(server, monkeypatch):
    monkeypatch.setattr(notes_ingest, "STAGING_RETRY_DELAY_S", 0)
    original = notes_ingest._write_file
    calls = {"n": 0}

    def flaky(path, data):
        calls["n"] += 1
        if calls["n"] <= 2:
            raise OSError(errno.EAGAIN, "Resource temporarily unavailable")
        return original(path, data)

    monkeypatch.setattr(notes_ingest, "_write_file", flaky)
    assert upload(server, fields())[0] == 202
    assert calls["n"] == 4  # two failures, then recording.wav and note.json


def test_a_persistent_transient_error_is_507_after_three_retries(server, monkeypatch):
    monkeypatch.setattr(notes_ingest, "STAGING_RETRY_DELAY_S", 0)
    calls = {"n": 0}

    def always_busy(path, data):
        calls["n"] += 1
        raise OSError(errno.EBUSY, "Device or resource busy")

    monkeypatch.setattr(notes_ingest, "_write_file", always_busy)
    status, payload = upload(server, fields())
    assert status == 507 and error_of(payload)["code"] == "storage"
    assert calls["n"] == 4  # the first try plus three retries (§5.7)
    assert not list((server.notes_dir / ".staging").glob("*"))


def test_a_paused_server_answers_503(server):
    server.set_paused(True)
    status, payload = upload(server, fields())
    assert status == 503
    assert error_of(payload)["code"] == "unavailable"
    assert error_of(payload)["retryable"] is True
    server.set_paused(False)
    assert upload(server, fields())[0] == 202


# -- the HTTP surface (§1, §3.2) -------------------------------------------------------


def test_route_matches_the_contract(server):
    client = TestClient(create_app(server))
    body_fields = fields(idempotency_key="route-key")
    body = multipart(body_fields)
    wire = signed(body_fields)
    wire["content-type"] = f"multipart/form-data; boundary={BOUNDARY}"
    response = client.post("/notes/v1", content=body, headers=wire)
    assert response.status_code == 202
    assert response.json()["status"] == "accepted"
    assert len(response.json()["job_id"]) == 64
    assert client.post("/notes/v1", content=body, headers=wire).json()["status"] == "duplicate"
    health = client.get("/health")
    assert health.status_code == 200
    assert health.json() == {"status": "ok", "port": None, "last_error": None,
                             "inbox": 1, "done": 0, "failed": 0}


def test_unknown_path_is_404_and_a_wrong_method_is_405(server):
    client = TestClient(create_app(server))
    missing = client.get("/notes/v2")
    assert missing.status_code == 404
    assert missing.json()["error"]["code"] == "not_found"
    wrong = client.get("/notes/v1")
    assert wrong.status_code == 405
    assert wrong.json()["error"]["code"] == "method_not_allowed"
    assert "POST" in wrong.headers.get("allow", "")


def test_the_page_is_served_beside_the_endpoint(server, tmp_path):
    """The container's one origin (deploy/bluetooth-sync): ``/`` is the phone's
    relay page and ``/notes/v1`` is still the contract, from the same app."""
    page = tmp_path / "relay"
    page.mkdir()
    (page / "index.html").write_text("<!doctype html><title>relay</title>", encoding="utf-8")
    (page / "relay.mjs").write_text("export const SERVICE_UUID = 1;\n", encoding="utf-8")
    client = TestClient(create_app(server, page))
    assert client.get("/").text == "<!doctype html><title>relay</title>"
    assert "SERVICE_UUID" in client.get("/relay.mjs").text
    assert client.get("/health").json()["status"] == "ok"
    body_fields = fields(idempotency_key="page-key")
    wire = signed(body_fields)
    wire["content-type"] = f"multipart/form-data; boundary={BOUNDARY}"
    assert client.post("/notes/v1", content=multipart(body_fields), headers=wire).status_code == 202


def test_stage_only_serves_the_endpoint_without_a_pipeline(tmp_path):
    """Without a transcriber the container still accepts notes: they are verified
    and staged in ``inbox/`` and nothing claims to have processed them."""
    staged = NotesServer(FakeStore(), tmp_path / "notes", host="127.0.0.1", port=0,
                         obsidian_dir=tmp_path / "vault", stage_only=True)
    assert staged.pipeline is None
    status, body = upload(staged, fields(idempotency_key="stage-key"))
    assert status == 202
    assert list((tmp_path / "notes" / "inbox").iterdir())
    assert not (tmp_path / "vault").exists() or not list((tmp_path / "vault").glob("*.md"))


def test_the_default_vault_folder_is_lowercase_at_the_vault_root(tmp_path, monkeypatch):
    """§5.4: one spelling, ``~/obsidian/voice notes``. ``Notes/voice notes`` and
    ``Voice Notes`` were both wrong on a case-sensitive host, and the mixed
    spelling split the vault's Syncthing pair."""
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    default = NotesServer(FakeStore(), tmp_path / "notes", host="127.0.0.1", port=0)
    assert default.pipeline.obsidian_dir == tmp_path / "home" / "obsidian" / "voice notes"


def test_the_route_answers_400_for_a_non_multipart_request(server):
    client = TestClient(create_app(server))
    response = client.post("/notes/v1", content=b"hello",
                           headers={"content-type": "application/octet-stream"})
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "bad_request"
