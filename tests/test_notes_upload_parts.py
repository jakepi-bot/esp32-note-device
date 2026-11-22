"""The part half of the voice-note upload (§2.5), driven through the endpoint.

A note longer than five minutes arrives as several requests. These cases pin the
behaviour the device relies on: the parts stage under one note's job, no vault
note appears until the set is complete, the last part assembles one
``recording.wav`` whose ``duration_seconds`` is the sum, a retried part is a
duplicate of itself and never doubles audio, and a part that arrives for a note
already stored is a duplicate of that note rather than a second one.

The audio is synthetic: a WAV of ``seconds`` at 8 kHz (16 MB would be 12 min at
24 kHz; the rule is time-based, so the rate only changes the bytes).
"""

from __future__ import annotations

import errno
import hashlib
import json
import time

import pytest

from hermes_gadget_plugin import notes_ingest, notes_parts
from hermes_gadget_plugin.notes_ingest import NotesServer
from test_notes_ingest import (  # the contract suite's fixtures and wire helpers
    BOUNDARY,
    DEVICE,
    KEY,
    RECORDED_AT,
    FakeStore,
    error_of,
    fields,
    multipart,
    signed,
    wav_bytes,
)

pytest.importorskip("fastapi")
pytest.importorskip("httpx")

NOTE_ID = "hg-0123456789abcdef-20261120T081504-0001"
RATE = 8000
CONTENT_TYPE = f"multipart/form-data; boundary={BOUNDARY}"


def notes_server(tmp_path, cls=NotesServer, **kwargs):
    return cls(FakeStore(), tmp_path / "notes", host="127.0.0.1", port=0,
               obsidian_dir=tmp_path / "vault",
               transcribe=lambda audio, meta: "irrelevant here",
               summarize=lambda text: "irrelevant here", **kwargs)


@pytest.fixture
def server(tmp_path):
    return notes_server(tmp_path)


class CountingServer(NotesServer):
    """Records every part that reached storage, and can fail one on purpose."""

    def __init__(self, *args, fail_part=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.seen: list[int] = []
        self.fail_part = fail_part

    def _store_part(self, entry, request):
        self.seen.append(request.part.index)
        if self.fail_part == request.part.index:
            self.fail_part = None
            raise OSError(errno.ENOSPC, "no space left on device")
        return super()._store_part(entry, request)


def note_audio(seconds: int):
    """A synthetic note and its parts: (bytes, WavLayout, parts, part bytes)."""
    data = wav_bytes(rate=RATE, frames=RATE * seconds)
    info = notes_parts.probe_wav(data)
    assert info is not None
    parts = notes_parts.plan_parts(info)
    return data, info, parts, [notes_parts.slice_part(data, info, part) for part in parts]


def part_fields(part, **overrides):
    body = {"device_id": DEVICE, "recorded_at": RECORDED_AT}
    body.update(notes_parts.part_fields(NOTE_ID, part))
    body.update(overrides)
    return {name: value for name, value in body.items() if value is not None}


def upload_part(server, part, blob, *, headers=None, body_fields=None, audio=None):
    body_fields = part_fields(part) if body_fields is None else body_fields
    audio = blob if audio is None else audio
    body = multipart(body_fields, audio, audio_name=f"part-{part.index}.wav")
    headers = signed(body_fields, audio) if headers is None else headers
    return server.handle_upload(headers, CONTENT_TYPE, len(body), body)


def complete_note(server, seconds=12 * 60):
    data, info, parts, blobs = note_audio(seconds)
    answers = [upload_part(server, part, blob) for part, blob in zip(parts, blobs)]
    return data, info, parts, answers


def job_id():
    return NotesServer.note_job_id(DEVICE, NOTE_ID)


# -- the set of parts ---------------------------------------------------------------


def test_two_of_three_parts_leave_the_note_partial_and_write_nothing(server):
    data, info, parts, blobs = note_audio(12 * 60)
    first = upload_part(server, parts[0], blobs[0])
    assert first[0] == 202
    assert first[1]["status"] == "accepted"
    assert first[1]["parts"] == 1 and first[1]["part_count"] == 3
    second = upload_part(server, parts[1], blobs[1])
    assert second[0] == 202
    assert second[1]["parts"] == 2

    entry = server.inbox / job_id()
    assert entry.is_dir()
    assert sorted(p.name for p in (entry / "parts").glob("*.wav")) == ["000.wav", "001.wav"]
    assert not (entry / "note.json").exists()
    assert not (entry / "recording.wav").exists()
    # It is a partial note, not an inbox note: the pipeline has nothing to fail.
    assert server.counts() == {"inbox": 0, "done": 0, "failed": 0, "partial": 1}
    assert server.pipeline.run_once() is None
    assert entry.is_dir() and not (entry / ".error").exists()
    assert list((server.notes_dir.parent / "vault").rglob("*.md")) == []


def test_the_last_part_assembles_one_wav_whose_audio_is_the_sum(server):
    data, info, parts, blobs = note_audio(12 * 60)
    for part, blob in zip(parts[:2], blobs[:2]):
        assert upload_part(server, part, blob)[0] == 202
    status, payload = upload_part(server, parts[2], blobs[2])
    assert status == 202
    assert payload["assembled"] is True
    assert payload["part_count"] == 3
    assert payload["duration_seconds"] == 720

    entry = server.inbox / job_id()
    recording = (entry / "recording.wav").read_bytes()
    assert recording == data  # header + every part's PCM, byte for byte
    assert not (entry / "parts").exists()  # the parts would only double the disk
    note = json.loads((entry / "note.json").read_text())
    assert note["duration_seconds"] == 720  # the sum, not the last part
    assert note["part_count"] == 3
    assert note["note_id"] == NOTE_ID
    assert note["audio_sha256"] == hashlib.sha256(recording).hexdigest()
    assert [p["index"] for p in note["parts"]] == [0, 1, 2]
    assert [p["duration_s"] for p in note["parts"]] == [300, 300, 120]
    assert [p["offset_s"] for p in note["parts"]] == [0, 300, 600]
    assert server.counts()["inbox"] == 1 and server.counts()["partial"] == 0


def test_a_duplicate_part_is_duplicate_and_does_not_double_the_audio(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    assert upload_part(server, parts[1], blobs[1])[0] == 202
    again = upload_part(server, parts[1], blobs[1])
    assert again == (200, {"status": "duplicate", "job_id": job_id(), "part_index": 1})
    manifest = json.loads((server.inbox / job_id() / "parts" / "manifest.json").read_text())
    assert len(manifest["parts"]) == 2
    assert upload_part(server, parts[2], blobs[2])[0] == 202
    assert (server.inbox / job_id() / "recording.wav").read_bytes() == data


def test_a_part_retry_uploads_only_that_part_and_the_audio_is_not_duplicated(tmp_path):
    server = notes_server(tmp_path, cls=CountingServer, fail_part=2)
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    assert upload_part(server, parts[1], blobs[1])[0] == 202
    failed = upload_part(server, parts[2], blobs[2])
    assert failed[0] == 507 and failed[1]["error"]["retryable"] is True
    assert not (server.inbox / job_id() / "note.json").exists()
    # The device retries the part that failed, not the note.
    retried = upload_part(server, parts[2], blobs[2])
    assert retried[0] == 202 and retried[1]["assembled"] is True
    assert server.seen == [0, 1, 2, 2]
    assert (server.inbox / job_id() / "recording.wav").read_bytes() == data


def test_parts_that_arrive_out_of_order_still_assemble_in_order(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[2], blobs[2])[0] == 202
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    status, payload = upload_part(server, parts[1], blobs[1])
    assert status == 202 and payload["assembled"] is True
    assert (server.inbox / job_id() / "recording.wav").read_bytes() == data


def test_a_part_for_a_completed_note_is_a_duplicate_of_that_note(server):
    data, info, parts, blobs = note_audio(12 * 60)
    complete_note(server)
    server.pipeline.run_once()
    assert server.counts()["done"] == 1
    answer = upload_part(server, parts[1], blobs[1])
    assert answer[0] == 200
    assert answer[1]["status"] == "duplicate"
    assert server.counts()["done"] == 1
    assert not (server.inbox / job_id()).exists()


def test_the_pipeline_writes_one_vault_note_with_the_summed_duration(server, tmp_path):
    data, info, parts, blobs = note_audio(12 * 60)
    complete_note(server)
    server.pipeline.run_once()
    assert server.counts() == {"inbox": 0, "done": 1, "failed": 0, "partial": 0}
    written = list((tmp_path / "vault").rglob("*.md"))
    assert len(written) == 1
    assert "duration_seconds: 720" in written[0].read_text(encoding="utf-8")


def test_health_reports_a_note_that_is_still_collecting_its_parts(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    health = server.health()
    assert health["note_parts"] == notes_ingest.PARTS_VERSION
    assert health["partial"] == 1 and health["inbox"] == 0


# -- what a part must be -------------------------------------------------------------


def test_a_part_without_its_siblings_is_422(server):
    data, info, parts, blobs = note_audio(12 * 60)
    fields_full = part_fields(parts[1])
    for missing in ("note_id", "part_index", "part_count", "part_offset"):
        body = {name: value for name, value in fields_full.items() if name != missing}
        status, payload = upload_part(server, parts[1], blobs[1], body_fields=body)
        assert status == 422, (missing, status, payload)
        assert error_of(payload)["field"] == missing


@pytest.mark.parametrize("overrides, field", [
    ({"part_index": "3"}, "part_index"),
    ({"part_index": "x"}, "part_index"),
    ({"part_count": "1"}, "part_count"),
    ({"part_count": "0"}, "part_count"),
    ({"part_count": "256"}, "part_count"),
    ({"part_offset": "301"}, "part_offset"),
    ({"duration_seconds": "301"}, "duration_seconds"),
    ({"note_id": "-bad"}, "note_id"),
    ({"idempotency_key": "some-other-key"}, "idempotency_key"),
    ({"idempotency_key": f"{NOTE_ID}/1"}, "idempotency_key"),
])
def test_a_part_the_contract_refuses_is_422(server, overrides, field):
    data, info, parts, blobs = note_audio(12 * 60)
    body = part_fields(parts[1], **overrides)
    status, payload = upload_part(server, parts[1], blobs[1], body_fields=body)
    assert status == 422, payload
    assert error_of(payload)["field"] == field


def test_a_part_must_be_wav_because_the_note_is_assembled_from_pcm(server):
    data, info, parts, blobs = note_audio(12 * 60)
    opus = (b"OggS" + b"\x00\x02" + (0).to_bytes(8, "little") + (1).to_bytes(4, "little")
            + (0).to_bytes(4, "little") + b"\x00\x00\x00\x00" + bytes([1, 19]) + b"OpusHead"
            + bytes([1, 1]) + (312).to_bytes(2, "little") + (24000).to_bytes(4, "little")
            + (0).to_bytes(2, "little") + bytes([0]))
    status, payload = upload_part(server, parts[0], opus)
    assert status == 422
    assert error_of(payload)["field"] == "audio"


def test_the_mac_covers_the_part_fields(server):
    data, info, parts, blobs = note_audio(12 * 60)
    body = part_fields(parts[0])
    # Signed as a v1 request: the fields were not part of what was signed.
    headers = signed(body, blobs[0], with_part=False)
    status, payload = upload_part(server, parts[0], blobs[0], headers=headers, body_fields=body)
    assert status == 401
    assert error_of(payload)["code"] == "auth"


def test_parts_of_one_note_must_agree_on_its_identity(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    other = "2026-11-20T09:15:04Z"
    body = part_fields(parts[1], recorded_at=other)
    status, payload = upload_part(server, parts[1], blobs[1], body_fields=body)
    assert status == 409
    assert error_of(payload)["code"] == "idempotency_conflict"


def test_reusing_a_part_index_with_different_audio_conflicts(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    other = bytearray(wav_bytes(rate=RATE, frames=RATE * 300))
    other[-1] ^= 0xFF  # the same geometry, different audio
    other = bytes(other)
    status, payload = upload_part(server, parts[0], other)
    assert status == 409
    assert payload["error"]["code"] == "idempotency_conflict"


# -- an orphaned set ---------------------------------------------------------------


def test_an_orphaned_set_is_swept_and_says_so(server, caplog):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    with caplog.at_level("INFO", logger="hermes_gadget_plugin.notes_ingest"):
        removed = server.sweep_parts(max_age_s=0, now=time.time() + 1)
    assert removed == 1
    assert not (server.inbox / job_id()).exists()
    assert server.counts()["partial"] == 0
    assert any("partial voice note" in record.message and "removed" in record.message
               for record in caplog.records)


def test_a_set_that_is_still_progressing_is_not_swept(server):
    data, info, parts, blobs = note_audio(12 * 60)
    assert upload_part(server, parts[0], blobs[0])[0] == 202
    assert server.sweep_parts(now=time.time() + 60) == 0
    assert (server.inbox / job_id()).is_dir()


# -- the boundary -------------------------------------------------------------------


def test_a_note_at_or_under_five_minutes_is_one_upload_with_no_part_fields(server):
    body = fields(idempotency_key=NOTE_ID)
    audio = wav_bytes(rate=RATE, frames=RATE * 300)
    body["duration_seconds"] = "300"
    status, payload = server.handle_upload(
        signed(body, audio), CONTENT_TYPE, None,
        multipart(body, audio))
    assert status == 202 and payload["status"] == "accepted"
    entry = server.inbox / payload["job_id"]
    assert (entry / "recording.wav").read_bytes() == audio
    assert "part_count" not in json.loads((entry / "note.json").read_text())
    assert server.counts() == {"inbox": 1, "done": 0, "failed": 0, "partial": 0}
