"""The offline voice-note pipeline against ``docs/voice-note-ingestion.md`` §5.

Every external is a double — transcription and summarization are injected, the
vault is a temporary directory — and the clock is a fixture's, so the backoff
schedule is asserted as numbers instead of sleeps. What is real here is the
contract: the filename (pinned to the host's timezone, DST-aware), the fixed
frontmatter keys in order, the transcript and summary in both places, the
audio-before-note atomic write, the dedupe scan, and the retryable-vs-terminal
taxonomy with the attempt cap.
"""

from __future__ import annotations

import errno
import hashlib
import json
import os
import re
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from hermes_gadget_plugin import notes_pipeline
from hermes_gadget_plugin.notes_pipeline import (
    MAX_ATTEMPTS,
    RETRY_BACKOFF_S,
    NoteMeta,
    NotesPipeline,
    PipelineError,
    Summary,
    classify,
    hermes_summarize,
)

JOB_ID = "3f9a1c7d4e5b6a8f0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2a3b"  # §5.6's example
DEVICE = "hg-0123456789abcdef"
RECORDED_UTC = "2026-11-20T14:15:04Z"  # 08:15 on a CST host — the contract's example
RECORDED_RAW = "2026-11-20T08:15:04-06:00"
AUDIO = b"RIFF-fake-wav-bytes"  # the pipeline hashes the audio, never parses it
AUDIO_SHA = hashlib.sha256(AUDIO).hexdigest()
TITLE = "Pick up the dry cleaning"
SUMMARY = "Dry cleaning is ready before five. Call Anna about the tickets."
KEY_POINTS = ("Dry cleaning ready before five", "Call Anna about the tickets")
TRANSCRIPT = "buy milk and call Anna about the tickets\nthe second line keeps its spacing"
NOTE_STEM = f"2026-11-20 {TITLE}"                       # date + the note's own title


@pytest.fixture(autouse=True)
def chicago_time():
    """Pin the host timezone so the filename asserts the contract's example
    instead of whatever the CI box's TZ happens to be."""
    if not hasattr(time, "tzset"):
        pytest.skip("tzset is POSIX-only")
    previous = os.environ.get("TZ")
    os.environ["TZ"] = "America/Chicago"
    time.tzset()
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = previous
        time.tzset()


class Clock:
    """A stand-in for time.time so backoff is asserted, not slept through."""

    def __init__(self, now: float = 1_900_000_000.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def make_pipeline(tmp_path: Path, *, transcribe=None, summarize=None,
                  clock: Clock | None = None) -> NotesPipeline:
    pipe = NotesPipeline(
        tmp_path / "notes", tmp_path / "vault",
        transcribe=transcribe or (lambda audio, meta: TRANSCRIPT),
        summarize=summarize or (lambda text: Summary(TITLE, SUMMARY, KEY_POINTS)),
        clock=clock or Clock())
    pipe.ensure_dirs()
    return pipe


def stage(pipe: NotesPipeline, *, job_id: str = JOB_ID, device: str = DEVICE,
          recorded_utc: str = RECORDED_UTC, recorded_raw: str = RECORDED_RAW,
          audio: bytes = AUDIO, sha: str | None = None, codec: str = "wav",
          attempts: int = 0, **extra) -> Path:
    """One inbox entry exactly as the endpoint stages it (§5.2)."""
    entry = pipe.inbox / job_id
    entry.mkdir(parents=True, exist_ok=True)
    (entry / f"recording.{codec}").write_bytes(audio)
    note = {"job_id": job_id, "device_id": device, "recorded_at": recorded_raw,
            "recorded_at_utc": recorded_utc, "duration_seconds": 8,
            "audio_sha256": sha if sha is not None else hashlib.sha256(audio).hexdigest(),
            "codec": codec, "supplied_key": None, "attempts": attempts,
            "last_error": None, "accepted_unix": 1795232104}
    note.update(extra)
    (entry / "note.json").write_text(json.dumps(note, indent=2, sort_keys=True), encoding="utf-8")
    return entry


def read_error(pipe: NotesPipeline, job_id: str = JOB_ID) -> dict:
    return json.loads((pipe.failed / job_id / ".error").read_text(encoding="utf-8"))


def attempts_of(entry: Path) -> int:
    return json.loads((entry / "note.json").read_text(encoding="utf-8"))["attempts"]


def refuse_note(real_replace, error, flag):
    """An os.replace that fails for the markdown file (the commit point) while
    ``flag`` says so — the tests flip the flag instead of undoing the patch."""
    def wrapper(src, dst, *args, **kwargs):
        if flag["on"] and str(dst).endswith(".md"):
            raise error
        return real_replace(src, dst, *args, **kwargs)
    return wrapper


# -- acceptance: the note itself -------------------------------------------------------

def test_the_contract_note_lands_in_the_voice_notes_folder(tmp_path):
    transcribed, summarized = [], []

    def fake_transcribe(audio, meta):
        transcribed.append((audio.name, meta.job_id))
        return TRANSCRIPT

    def fake_summarize(text):
        summarized.append(text)
        return Summary(TITLE, SUMMARY, KEY_POINTS)

    pipe = make_pipeline(tmp_path, transcribe=fake_transcribe, summarize=fake_summarize)
    stage(pipe)
    assert pipe.run_once() is None  # nothing left waiting

    notes = list((tmp_path / "vault").glob("*.md"))
    assert [n.name for n in notes] == [NOTE_STEM + ".md"]
    text = notes[0].read_text(encoding="utf-8")

    # the frontmatter keys are fixed, in the contract's order (§5.6)
    frontmatter = text.split("---", 2)[1]
    assert re.findall(r"^([a-z_0-9]+):", frontmatter, re.M) == [
        "idempotency_key", "device_id", "recorded_at", "duration_seconds", "codec",
        "audio_sha256", "audio", "title", "transcript", "summary", "key_points", "tags"]
    assert f"idempotency_key: {JOB_ID}" in text
    assert f"device_id: {DEVICE}" in text
    assert "recorded_at: 2026-11-20T14:15:04Z" in text
    assert "duration_seconds: 8" in text
    assert "codec: wav" in text
    assert f"audio_sha256: {AUDIO_SHA}" in text
    assert f"audio: _audio/{NOTE_STEM}.wav" in text
    assert f"title: {TITLE}" in text
    assert "tags: [voice-note]" in text

    # the transcript keeps its lines and spacing in the frontmatter block
    first, second = TRANSCRIPT.splitlines()
    assert f"transcript: |\n  {first}\n  {second}" in text
    assert f"summary: |\n  {SUMMARY}" in text
    assert f"key_points:\n  - {KEY_POINTS[0]}\n  - {KEY_POINTS[1]}" in text

    # and the body a reader sees
    assert f"# {TITLE}" in text
    assert f"## Summary\n\n{SUMMARY}" in text
    assert "\n".join(f"- {point}" for point in KEY_POINTS) in text
    assert text.split("## Transcript", 1)[1].strip() == TRANSCRIPT

    # both steps ran, with exactly what they were given
    assert transcribed == [("recording.wav", JOB_ID)]
    assert summarized == [TRANSCRIPT]

    # the audio copy is byte-identical, the entry closed, no temp files left
    assert (tmp_path / "vault" / "_audio" / f"{NOTE_STEM}.wav").read_bytes() == AUDIO
    assert not any(pipe.inbox.iterdir())
    assert (pipe.done / JOB_ID).is_dir()
    assert not list((tmp_path / "vault").rglob("*.tmp-*"))
    assert pipe.run_once() is None
    assert len(list((tmp_path / "vault").glob("*.md"))) == 1


def test_a_summer_timestamp_uses_daylight_time(tmp_path):
    pipe = make_pipeline(tmp_path)
    stage(pipe, recorded_utc="2026-07-20T14:15:04Z",
          recorded_raw="2026-07-20T09:15:04-05:00")
    pipe.run_once()
    assert [p.name for p in (tmp_path / "vault").glob("*.md")] == \
        [f"2026-07-20 {TITLE}.md"]  # the local date, one day/zone off the winter stamp


def test_a_title_longer_than_sixty_characters_is_capped(tmp_path):
    pipe = make_pipeline(tmp_path, summarize=lambda text: Summary(
        "A title that runs far past sixty characters and must be cut down to size",
        SUMMARY, ()))
    stage(pipe)
    pipe.run_once()
    text = next((tmp_path / "vault").glob("*.md")).read_text(encoding="utf-8")
    title = next(line for line in text.splitlines() if line.startswith("title: "))[len("title: "):]
    assert len(title) <= 60
    assert title == "A title that runs far past sixty characters and must be"  # word boundary, not mid-word
    assert f"# {title}" in text


def test_a_hostile_title_is_quoted_so_the_frontmatter_stays_parseable(tmp_path):
    hostile = 'Note: "urgent" #1 - buy milk'
    pipe = make_pipeline(tmp_path, summarize=lambda text: Summary(hostile, SUMMARY, ()))
    stage(pipe)
    pipe.run_once()
    text = next((tmp_path / "vault").glob("*.md")).read_text(encoding="utf-8")
    assert f"title: {json.dumps(hostile)}" in text  # double-quoted, exactly as JSON writes it
    assert f"# {hostile}" in text  # the body keeps the real title


# -- acceptance: the filename is a title, not an id (§5.5) ------------------------------

def test_a_hostile_title_never_reaches_the_filename(tmp_path):
    """The frontmatter keeps the real title; the filename carries only what a
    shell, Windows and Syncthing all accept, so a note is findable and portable."""
    hostile = 'Note: "urgent" #1 - buy/milk? *now*'
    pipe = make_pipeline(tmp_path, summarize=lambda text: Summary(hostile, SUMMARY, ()))
    stage(pipe)
    pipe.run_once()
    names = [p.name for p in pipe.obsidian_dir.glob("*.md")]
    assert names == ["2026-11-20 Note urgent 1 - buy milk now.md"]
    assert set(names[0]) <= set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-_")


def test_a_filename_longer_than_sixty_characters_is_cut_at_a_word(tmp_path):
    long_title = "A title that runs far past sixty characters and must be cut down to size"
    pipe = make_pipeline(tmp_path, summarize=lambda text: Summary(long_title, SUMMARY, ()))
    stage(pipe)
    pipe.run_once()
    stem = next(pipe.obsidian_dir.glob("*.md")).stem
    assert stem == "2026-11-20 A title that runs far past sixty characters and must be"
    assert len(stem) - len("2026-11-20 ") <= 60


def test_a_title_with_nothing_usable_still_names_the_note(tmp_path):
    pipe = make_pipeline(tmp_path, summarize=lambda text: Summary("!!!", SUMMARY, ()))
    stage(pipe)
    pipe.run_once()
    assert [p.name for p in pipe.obsidian_dir.glob("*.md")] == ["2026-11-20 Voice note.md"]


# -- acceptance: duplicate handling -----------------------------------------------------

def test_a_note_already_written_for_this_job_is_not_written_again(tmp_path):
    pipe = make_pipeline(
        tmp_path, transcribe=lambda *a: pytest.fail("the dedupe scan must run before STT"))
    stage(pipe)
    existing = pipe.obsidian_dir / f"{NOTE_STEM}.md"
    existing.write_text(
        f"---\nidempotency_key: {JOB_ID}\ndevice_id: {DEVICE}\nrecorded_at: {RECORDED_UTC}\n"
        f"audio_sha256: {AUDIO_SHA}\n---\n\n# an earlier run's note\n", encoding="utf-8")
    before = existing.read_bytes()

    pipe.run_once()

    assert existing.read_bytes() == before  # untouched, not rewritten
    assert len(list(pipe.obsidian_dir.glob("*.md"))) == 1
    assert not any(pipe.inbox.iterdir())
    assert (pipe.done / JOB_ID).is_dir()


def test_the_same_recording_under_another_job_id_closes_without_a_second_note(tmp_path):
    pipe = make_pipeline(
        tmp_path, transcribe=lambda *a: pytest.fail("content dedupe must run before STT"))
    stage(pipe)  # a different job id carrying the same device, timestamp and bytes (§5.3)
    other = pipe.obsidian_dir / "someone-elses-name.md"
    other.write_text(
        f"---\nidempotency_key: {'bb' * 32}\ndevice_id: {DEVICE}\nrecorded_at: {RECORDED_UTC}\n"
        f"audio_sha256: {AUDIO_SHA}\n---\n\n# earlier\n", encoding="utf-8")

    pipe.run_once()

    assert [p.name for p in pipe.obsidian_dir.glob("*.md")] == ["someone-elses-name.md"]
    assert not any(pipe.inbox.iterdir())
    assert (pipe.done / JOB_ID).is_dir()


def test_a_note_whose_transcript_makes_it_huge_still_dedupes(tmp_path):
    pipe = make_pipeline(
        tmp_path, transcribe=lambda *a: pytest.fail("the dedupe scan must run before STT"))
    stage(pipe)
    giant = "word " * 30000  # ~150 KB inside the frontmatter block, far past any fixed head read
    existing = pipe.obsidian_dir / f"{NOTE_STEM}.md"
    existing.write_text(
        f"---\nidempotency_key: {JOB_ID}\ndevice_id: {DEVICE}\nrecorded_at: {RECORDED_UTC}\n"
        f"audio_sha256: {AUDIO_SHA}\ntranscript: |\n  {giant}\n---\n\n# old\n", encoding="utf-8")

    pipe.run_once()

    assert len(list(pipe.obsidian_dir.glob("*.md"))) == 1
    assert (pipe.done / JOB_ID).is_dir()


def test_a_short_id_clash_with_a_different_key_gets_a_suffix(tmp_path):
    clash_job = "3f9a1c7d" + "b" * 56  # same first 8 hex chars, a different job (§5.5)
    pipe = make_pipeline(tmp_path)
    stage(pipe)
    name = f"{NOTE_STEM}.md"
    clashing = pipe.obsidian_dir / name
    clashing.write_text(
        f"---\nidempotency_key: {clash_job}\ndevice_id: {DEVICE}\nrecorded_at: {RECORDED_UTC}\n"
        f"audio_sha256: {'cc' * 32}\n---\n\n# the other note\n", encoding="utf-8")

    pipe.run_once()

    assert clashing.read_text(encoding="utf-8").startswith(f"---\nidempotency_key: {clash_job}")
    written = pipe.obsidian_dir / f"{NOTE_STEM} (2).md"
    assert written.is_file()
    assert f"idempotency_key: {JOB_ID}" in written.read_text(encoding="utf-8")
    assert (pipe.obsidian_dir / "_audio" / f"{NOTE_STEM} (2).wav").read_bytes() == AUDIO


# -- acceptance: atomic write -----------------------------------------------------------

def test_a_failure_before_the_write_leaves_no_partial_note(tmp_path):
    def explodes(text):
        raise RuntimeError("the model exploded")

    pipe = make_pipeline(tmp_path, summarize=explodes)
    entry = stage(pipe)

    assert pipe.run_once() == pytest.approx(30)  # retryable: one backoff step
    assert not list((tmp_path / "vault").glob("*.md"))
    assert not list((tmp_path / "vault").rglob("*.tmp-*"))
    assert not list((tmp_path / "vault" / "_audio").glob("*"))
    assert (pipe.inbox / JOB_ID).is_dir()
    assert attempts_of(entry) == 1
    assert "the model exploded" in json.loads(
        (entry / "note.json").read_text(encoding="utf-8"))["last_error"]


def test_the_markdown_replace_is_the_commit_point(tmp_path, monkeypatch):
    pipe = make_pipeline(tmp_path)
    entry = stage(pipe)
    failing = {"on": True}
    monkeypatch.setattr(notes_pipeline.os, "replace",
                        refuse_note(notes_pipeline.os.replace,
                                    OSError(errno.EIO, "mid-write I/O error"), failing))

    assert pipe.run_once() == pytest.approx(30)  # EIO is unclassified: it retries
    assert not list((tmp_path / "vault").glob("*.md"))
    assert not list((tmp_path / "vault").rglob("*.tmp-*"))  # both temp files cleaned up
    assert (tmp_path / "vault" / "_audio" / f"{NOTE_STEM}.wav").read_bytes() == AUDIO
    assert attempts_of(entry) == 1

    failing["on"] = False  # the disk behaves again: the retry converges
    pipe.clock.advance(31)
    assert pipe.run_once() is None
    assert [p.name for p in (tmp_path / "vault").glob("*.md")] == [NOTE_STEM + ".md"]
    assert (pipe.done / JOB_ID).is_dir()


def test_a_note_committed_but_entry_left_behind_closes_without_a_second_note(tmp_path, monkeypatch):
    transcribed = []

    def fake_transcribe(audio, meta):
        transcribed.append(audio.name)
        return TRANSCRIPT

    pipe = make_pipeline(tmp_path, transcribe=fake_transcribe)
    entry = stage(pipe)
    real_move = NotesPipeline._move
    calls = {"n": 0}

    def crash_on_done(self, path, target_dir):
        calls["n"] += 1
        if calls["n"] == 1:  # the first close attempt dies between steps 3 and 5 (§5.7)
            raise RuntimeError("crash after the markdown replace")
        return real_move(self, path, target_dir)

    monkeypatch.setattr(NotesPipeline, "_move", crash_on_done)

    assert pipe.run_once() == pytest.approx(30)
    assert [p.name for p in (tmp_path / "vault").glob("*.md")] == [NOTE_STEM + ".md"]
    assert (pipe.inbox / JOB_ID).is_dir() and attempts_of(entry) == 1

    pipe.clock.advance(31)
    assert pipe.run_once() is None  # the dedupe scan finds the committed note

    assert (pipe.done / JOB_ID).is_dir()
    assert len(list((tmp_path / "vault").glob("*.md"))) == 1
    assert transcribed == ["recording.wav"]  # STT ran once; the retry never re-ran it


def test_stale_temp_files_are_swept_and_fresh_ones_are_left_alone(tmp_path):
    pipe = make_pipeline(tmp_path)
    stale = pipe.obsidian_dir / "wrecked.md.tmp-00000000"
    stale.write_text("crash debris", encoding="utf-8")
    stale_audio = pipe.obsidian_dir / "_audio" / "wrecked.wav.tmp-00000000"
    stale_audio.write_bytes(b"crash debris")
    old = time.time() - 25 * 3600
    os.utime(stale, (old, old))
    os.utime(stale_audio, (old, old))
    in_flight = pipe.obsidian_dir / "inflight.md.tmp-00000001"
    in_flight.write_text("a write in progress", encoding="utf-8")

    pipe.run_once()

    assert not stale.exists() and not stale_audio.exists()  # §5.7: older than 24 h
    assert in_flight.exists()  # fresh debris belongs to a write that may still land


# -- acceptance: error classification ---------------------------------------------------

class Http401(Exception):
    status_code = 401


class Http429(Exception):
    status_code = 429


class Http503(Exception):
    status_code = 503


def test_failures_classify_against_the_taxonomy():
    def check(exc, step, *, retryable, code):
        got = classify(exc, step)
        assert (got.retryable, got.code, got.step) == (retryable, code, step)

    # unclassified failures retry too — the attempt cap bounds them (§5.8)
    check(RuntimeError("whisper exploded"), "stt", retryable=True, code="unclassified")
    check(ConnectionError("refused"), "stt", retryable=True, code="connect")
    check(TimeoutError("too slow"), "summarize", retryable=True, code="timeout")
    # vault writes classify on errno
    check(OSError(errno.ENOSPC, "no space left"), "write", retryable=True, code="enospc")
    check(OSError(errno.EAGAIN, "try again"), "write", retryable=True, code="eagain")
    check(OSError(errno.EACCES, "denied"), "write", retryable=False, code="eacces")
    check(OSError(errno.EROFS, "read-only"), "write", retryable=False, code="erofs")
    # provider statuses: 4xx (except 429) terminal, 429/5xx retryable
    check(Http401("nope"), "stt", retryable=False, code="http_401")
    check(Http429("slow down"), "stt", retryable=True, code="http_429")
    check(Http503("down"), "summarize", retryable=True, code="http_503")

    class Requests500(Exception):
        response: object = None

    response_error = Requests500("boom")
    response_error.response = SimpleNamespace(status_code=500)
    check(response_error, "stt", retryable=True, code="http_500")

    # an already-classified error keeps its verdict and gains the step
    classified = classify(PipelineError("no model access", step=None,
                                        code="model_unavailable", retryable=False), "summarize")
    assert (classified.retryable, classified.code, classified.step) == \
        (False, "model_unavailable", "summarize")


def test_a_provider_rejection_quarantines_on_the_spot(tmp_path):
    def unauthorized(audio, meta):
        raise Http401("the model key is wrong")

    pipe = make_pipeline(tmp_path, transcribe=unauthorized)
    stage(pipe)
    assert pipe.run_once() is None  # terminal: no schedule, no retries
    assert not any(pipe.inbox.iterdir())
    assert read_error(pipe) == {"code": "http_401", "step": "stt",
                                "message": "the model key is wrong", "attempts": 1}


def test_a_vault_that_refuses_the_note_is_terminal(tmp_path, monkeypatch):
    pipe = make_pipeline(tmp_path)
    stage(pipe)
    monkeypatch.setattr(notes_pipeline.os, "replace",
                        refuse_note(notes_pipeline.os.replace,
                                    PermissionError(errno.EACCES, "vault read-only"),
                                    {"on": True}))

    assert pipe.run_once() is None  # EACCES: this path will never work
    assert not any(pipe.inbox.iterdir())
    assert read_error(pipe) == {"code": "eacces", "step": "write",
                                "message": "[Errno 13] vault read-only", "attempts": 1}
    assert not list((tmp_path / "vault").glob("*.md"))


def test_an_empty_transcript_is_terminal(tmp_path):
    pipe = make_pipeline(tmp_path, transcribe=lambda audio, meta: "   ")
    stage(pipe)
    assert pipe.run_once() is None
    error = read_error(pipe)
    assert error["code"] == "empty_transcript" and error["step"] == "stt"
    assert error["attempts"] == 1
    assert not list((tmp_path / "vault").glob("*.md"))


def test_staged_bytes_that_no_longer_match_the_checksum_are_terminal(tmp_path):
    def must_not_run(audio, meta):
        raise AssertionError("STT must not run on unverifiable audio")

    pipe = make_pipeline(tmp_path, transcribe=must_not_run)
    stage(pipe, sha="0" * 64)
    assert pipe.run_once() is None
    error = read_error(pipe)
    assert error["code"] == "checksum_mismatch" and error["step"] == "verify"
    assert error["attempts"] == 1


def test_a_missing_recording_file_is_terminal(tmp_path):
    pipe = make_pipeline(tmp_path)
    entry = stage(pipe)
    (entry / "recording.wav").unlink()
    assert pipe.run_once() is None
    assert read_error(pipe)["code"] == "missing_audio"


def test_note_json_that_fails_the_contract_is_terminal(tmp_path):
    pipe = make_pipeline(tmp_path)
    without_key = stage(pipe, job_id="0" * 64)
    note = json.loads((without_key / "note.json").read_text(encoding="utf-8"))
    del note["audio_sha256"]
    (without_key / "note.json").write_text(json.dumps(note), encoding="utf-8")
    unreadable = stage(pipe, job_id="1" * 64)
    (unreadable / "note.json").write_text("{not json at all", encoding="utf-8")

    pipe.run_once()

    for job_id in ("0" * 64, "1" * 64):
        error = read_error(pipe, job_id)
        assert error["code"] == "invalid_metadata" and error["step"] == "verify"
        assert error["attempts"] == 1


def test_a_retryable_failure_backs_off_and_the_eighth_attempt_quarantines(tmp_path):
    clock = Clock()

    def always_fails(audio, meta):
        raise RuntimeError("whisper exploded")

    pipe = make_pipeline(tmp_path, transcribe=always_fails, clock=clock)
    entry = stage(pipe)

    for attempt, expected in enumerate(RETRY_BACKOFF_S, start=1):
        assert pipe.run_once() == pytest.approx(expected)  # 30, 60, 120, … 1920
        assert attempts_of(entry) == attempt
        assert not list((tmp_path / "vault").glob("*.md"))
        assert pipe.run_once() == pytest.approx(expected)  # before the backoff elapses: no work
        assert attempts_of(entry) == attempt
        clock.advance(expected + 1)

    assert pipe.run_once() is None  # the eighth failure hits the cap
    assert not any(pipe.inbox.iterdir())
    assert read_error(pipe) == {"code": "attempts_exhausted", "step": "stt",
                                "message": "whisper exploded", "attempts": MAX_ATTEMPTS}
    assert attempts_of(pipe.failed / JOB_ID) == MAX_ATTEMPTS
    assert pipe.run_once() is None  # nothing left to do


def test_the_attempt_count_survives_a_restart(tmp_path):
    clock = Clock()

    def always_fails(audio, meta):
        raise RuntimeError("whisper exploded")

    stage(make_pipeline(tmp_path, clock=clock))
    first = make_pipeline(tmp_path, transcribe=always_fails, clock=clock)
    assert first.run_once() == pytest.approx(30)  # attempt 1

    # a gateway restart clears the in-memory schedule but not the count (§5.2)
    restarted = make_pipeline(tmp_path, transcribe=always_fails, clock=clock)
    assert restarted.run_once() == pytest.approx(60)  # attempt 2, backoff continuing
    assert attempts_of(restarted.inbox / JOB_ID) == 2

    # and once the model recovers, the job still completes
    recovered = make_pipeline(tmp_path, clock=clock)
    clock.advance(121)
    assert recovered.run_once() is None
    assert [p.name for p in recovered.obsidian_dir.glob("*.md")] == [NOTE_STEM + ".md"]
    assert (recovered.done / JOB_ID).is_dir()


def test_junk_dot_directories_in_the_inbox_are_left_alone(tmp_path):
    pipe = make_pipeline(tmp_path)
    junk = pipe.inbox / ".staging"
    junk.mkdir()
    (junk / "note-half-written").write_text("x", encoding="utf-8")
    stage(pipe)

    pipe.run_once()

    assert junk.is_dir()  # only real jobs are attempted
    assert (pipe.done / JOB_ID).is_dir()


# -- the model step ---------------------------------------------------------------------

class FakeLlm:
    def __init__(self, text: str) -> None:
        self.text = text
        self.calls = []

    def complete(self, messages, **kwargs):
        self.calls.append((messages, kwargs))
        return SimpleNamespace(text=self.text)


def test_the_model_step_asks_for_json_and_parses_the_answer():
    llm = FakeLlm(json.dumps({"title": TITLE, "summary": SUMMARY,
                              "key_points": list(KEY_POINTS)}))
    result = hermes_summarize(llm, TRANSCRIPT)
    assert result == Summary(TITLE, SUMMARY, KEY_POINTS)
    messages, kwargs = llm.calls[0]
    assert messages[0]["role"] == "system" and '"key_points"' in messages[0]["content"]
    assert messages[1] == {"role": "user", "content": TRANSCRIPT}
    assert kwargs["purpose"] == "voice note summary"


@pytest.mark.parametrize("text", [
    "",  # empty model output
    "the model rambled instead of answering",  # unparseable
    '{"title": "only a title"}',  # no summary in the JSON
])
def test_empty_or_unparseable_model_output_is_retryable(text):
    with pytest.raises(PipelineError) as caught:
        hermes_summarize(FakeLlm(text), TRANSCRIPT)
    assert caught.value.retryable is True
    assert caught.value.code == "unparseable_output"
    assert caught.value.step == "summarize"


def test_json_the_model_fenced_still_parses():
    llm = FakeLlm('```json\n{"title": "T", "summary": "S.", "key_points": ["a"]}\n```')
    assert hermes_summarize(llm, TRANSCRIPT) == Summary("T", "S.", ("a",))


def test_a_hermes_without_model_access_is_terminal_config():
    with pytest.raises(PipelineError) as caught:
        hermes_summarize(None, TRANSCRIPT)
    assert caught.value.retryable is False
    assert caught.value.code == "model_unavailable"


def test_the_summarize_step_may_return_bare_text(tmp_path):
    pipe = make_pipeline(tmp_path, summarize=lambda text: "A short summary.")
    stage(pipe)
    pipe.run_once()
    text = next((tmp_path / "vault").glob("*.md")).read_text(encoding="utf-8")
    assert "title: A short summary." in text  # the title is derived, still one line, still ≤60
    assert "summary: |\n  A short summary." in text
    assert "key_points: []" in text


@pytest.mark.parametrize("answer", [42, "", {"summary": ""}])
def test_a_summarize_answer_without_a_summary_is_retryable(tmp_path, answer):
    pipe = make_pipeline(tmp_path, summarize=lambda text: answer)
    entry = stage(pipe)
    assert pipe.run_once() == pytest.approx(30)
    assert attempts_of(entry) == 1
    assert not list((tmp_path / "vault").glob("*.md"))


# -- metadata ---------------------------------------------------------------------------

def test_note_meta_reads_the_staged_contract(tmp_path):
    pipe = make_pipeline(tmp_path)
    entry = stage(pipe, attempts=3, supplied_key="firm-0001")

    meta = NoteMeta.from_note_json(entry)

    assert (meta.job_id, meta.device_id, meta.recorded_at_utc) == (JOB_ID, DEVICE, RECORDED_UTC)
    assert (meta.duration_seconds, meta.audio_sha256, meta.codec) == (8, AUDIO_SHA, "wav")
    assert meta.attempts == 3


def test_a_raw_recorded_at_without_the_normalized_stamp_is_normalized(tmp_path):
    pipe = make_pipeline(tmp_path)
    entry = stage(pipe)
    note = json.loads((entry / "note.json").read_text(encoding="utf-8"))
    del note["recorded_at_utc"]
    (entry / "note.json").write_text(json.dumps(note), encoding="utf-8")

    meta = NoteMeta.from_note_json(entry)

    assert meta.recorded_at_utc == RECORDED_UTC  # 08:15:04-06:00 == 14:15:04Z


@pytest.mark.parametrize("recorded", ["not a date", "2026-11-20T14:15:04"])  # no offset
def test_a_timestamp_that_cannot_be_normalized_is_terminal(tmp_path, recorded):
    pipe = make_pipeline(tmp_path)
    entry = stage(pipe, recorded_raw=recorded)
    note = json.loads((entry / "note.json").read_text(encoding="utf-8"))
    del note["recorded_at_utc"]
    (entry / "note.json").write_text(json.dumps(note), encoding="utf-8")

    assert pipe.run_once() is None
    assert read_error(pipe)["code"] == "invalid_metadata"
