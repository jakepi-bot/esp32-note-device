"""Offline voice-note pipeline: inbox entry -> transcript -> summary -> Obsidian note.

The upload endpoint (``notes_ingest``) answers 202 as soon as the recording is
durably in ``notes/inbox/<job_id>/``; this module does the slow work afterwards,
on a worker thread, so the device never waits on a model round-trip. One pass
(``run_once``) processes every inbox entry that is due and reports when the next
one becomes due; the server wakes it on each upload and polls as a safety net.

The contract is ``docs/voice-note-ingestion.md`` §5: the filename
``YYYY-MM-DD_HHMM_<device-id>_<short-id>.md`` from ``recorded_at`` in the host's
local timezone, the fixed frontmatter keys, the dedupe scan before any write,
the atomic audio-then-markdown ordering (§5.7), and the retryable-vs-terminal
taxonomy with ``min(30 s * 2**(n-1), 3600 s)`` backoff over 8 attempts (§5.8).

Host-side failure handling, never the device's: a retryable failure is recorded
on the entry (``note.json`` ``attempts``/``last_error``) and retried with
backoff; a terminal failure — or the eighth attempt — quarantines the entry into
``notes/failed/`` with ``.error`` holding ``{code, step, message, attempts}``.
An entry only leaves ``inbox/`` once its note is written, and a note is only
written once (the dedupe scan reads frontmatter before writing), so a crash at
any point converges to exactly one Obsidian note per job.

Everything Hermes-specific (speech-to-text, the model) is injected or imported
lazily: the pure tests run without a Hermes checkout, the gateway gets the same
transcription path its voice messages use.
"""

from __future__ import annotations

import errno
import json
import logging
import os
import re
import shutil
import time
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Optional

logger = logging.getLogger(__name__)

#: Total attempts per job (1 + 7 retries); the eighth failure quarantines.
MAX_ATTEMPTS = 8
#: Delay before retry n (n = 1..7): min(30 s * 2**(n-1), 3600 s) — §5.8.
RETRY_BACKOFF_S = tuple(min(30 * 2 ** (n - 1), 3600) for n in range(1, MAX_ATTEMPTS))
#: How often the worker re-checks the inbox when nothing is due (files can appear
#: from outside, e.g. a mounted restore of the plugin-data directory).
IDLE_POLL_S = 120
SUMMARIZE_TIMEOUT_S = 120
#: Crash debris the idle pass sweeps from the vault (§5.7).
TMP_MAX_AGE_S = 24 * 3600
#: ``title`` is model-written but capped (§5.6).
TITLE_MAX = 60
AUDIO_DIRNAME = "_audio"

_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_DEVICE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_UTC_STAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
#: A transcript lives inside the frontmatter, so the scan streams to the
#: closing ``---`` instead of reading a fixed head that long notes outgrow.
_FRONTMATTER_MAX_BYTES = 4 << 20
_FM_FIELD = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):(?:[ \t](.*))?$")
#: A value safe to write as a bare YAML scalar; anything else is double-quoted
#: (a JSON string, which is valid YAML). A colon is safe: it only ends a plain
#: scalar when followed by a space, which the guard below rejects.
_PLAIN_SAFE = re.compile(r"^[A-Za-z0-9_./][A-Za-z0-9_ .,!?()'/&+:-]*$")

#: Write errors that mean "try again" and errors that mean "this path will
#: never work" (§5.8): ENOSPC/EAGAIN/EBUSY/EINTR and unknown errnos retry,
#: EACCES/EPERM/EROFS/EINVAL/ENOENT quarantine. Everything unclassified retries
#: too — the attempt cap bounds it.
_TERMINAL_ERRNOS = {errno.EACCES, errno.EPERM, errno.EROFS, errno.EINVAL, errno.ENOENT}

SUMMARY_SYSTEM = (
    "You summarize offline voice notes dictated on a small gadget. The user message is a speech "
    "transcript and may contain recognition errors. Reply only with a JSON object with exactly these "
    'keys: "title" (at most 60 characters, what the note is about), "summary" (one to three '
    'sentences: what the note is about and anything it asks for) and "key_points" (an array of short '
    "strings: the facts or action items worth listing). Reply in the transcript's language. No "
    "preamble, no Markdown, no code fences.")


class PipelineError(RuntimeError):
    """A pipeline step failed, classified against the contract's taxonomy (§5.8).

    ``retryable`` decides the fate: False quarantines the entry immediately,
    True schedules a backoff retry until the attempt cap. ``code`` and ``step``
    are what ``failed/<job_id>/.error`` records.
    """

    def __init__(self, message: str, *, step: Optional[str] = None,
                 code: str = "pipeline_error", retryable: bool = True) -> None:
        super().__init__(message)
        self.step = step
        self.code = code
        self.retryable = retryable


def classify(exc: BaseException, step: str) -> PipelineError:
    """Map any failure onto the retryable-vs-terminal taxonomy (§5.8).

    Provider failures carrying an HTTP status classify on it (4xx other than
    429 terminal, 429/5xx retryable), vault writes classify on ``errno``,
    connection and timeout errors retry, and anything unclassified retries too —
    bounded by the attempt cap.
    """
    if isinstance(exc, PipelineError):
        return PipelineError(str(exc), step=exc.step or step, code=exc.code,
                             retryable=exc.retryable)
    if isinstance(exc, TimeoutError):  # before OSError: both are OSError subclasses
        return PipelineError(str(exc) or "timed out", step=step, code="timeout", retryable=True)
    if isinstance(exc, ConnectionError):
        return PipelineError(str(exc) or "connection failed", step=step, code="connect", retryable=True)
    if isinstance(exc, OSError):
        name = (errno.errorcode.get(exc.errno or 0) or "os_error").lower()
        retryable = exc.errno not in _TERMINAL_ERRNOS  # ENOSPC/EAGAIN/… and unknown errnos retry
        return PipelineError(str(exc) or name, step=step, code=name, retryable=retryable)
    status = _status_code(exc)
    if status is not None:
        retryable = not (400 <= status < 500 and status != 429)
        return PipelineError(str(exc) or f"HTTP {status}", step=step,
                             code=f"http_{status}", retryable=retryable)
    return PipelineError(str(exc) or type(exc).__name__, step=step,
                         code="unclassified", retryable=True)


def _status_code(exc: BaseException) -> Optional[int]:
    """The HTTP status an exception carries, in any of the usual shapes."""
    for attr in ("status_code", "status", "code"):
        value = getattr(exc, attr, None)
        if isinstance(value, int) and not isinstance(value, bool) and 100 <= value <= 599:
            return value
    value = getattr(getattr(exc, "response", None), "status_code", None)
    if isinstance(value, int) and not isinstance(value, bool) and 100 <= value <= 599:
        return value
    return None


# -- Hermes-backed steps (imported lazily so the pure tests need no Hermes) ------------

def hermes_transcribe(audio_path: Path, language: Optional[str] = None) -> str:
    """The transcript of ``audio_path`` through Hermes's speech-to-text.

    ``language`` (``platforms.gadget.extra.stt_language``) is handed to the
    ``pre_transcription`` hook the same way the adapter does it for live gadget
    recordings; the file stem identifies the clip, so the audio is copied to a
    uniquely named temporary file first — every inbox entry is called
    ``recording.<ext>``. An empty result is returned as ``''``: the pipeline
    classifies an empty transcript as terminal (§5.8), retrying cannot help.
    """
    from . import runtime

    try:
        from tools.transcription_tools import transcribe_audio
    except ImportError as exc:  # no Hermes transcription tooling: config, not transient
        raise PipelineError(f"speech-to-text is unavailable: {exc}", step="stt",
                            code="stt_unavailable", retryable=False) from exc

    tmp_dir: Optional[Path] = None
    path = audio_path
    if language:
        import tempfile

        tmp_dir = Path(tempfile.mkdtemp(prefix="hg-note-stt-"))
        path = tmp_dir / f"{audio_path.parent.name}{audio_path.suffix}"
        shutil.copy2(audio_path, path)
        runtime.note_clip(str(path), language)
    try:
        result = transcribe_audio(str(path), source="gateway") or {}
    finally:
        if tmp_dir is not None:
            shutil.rmtree(tmp_dir, ignore_errors=True)
    if result.get("error"):
        raise PipelineError(str(result["error"]), step="stt", code="stt_failed", retryable=True)
    return str(result.get("transcript") or "")


def hermes_summarize(llm: Any, transcript: str) -> "Summary":
    """The model's title, summary and key points for ``transcript``.

    Empty or unparseable model output is a retryable failure (§5.8); a Hermes
    that gives plugins no model access is missing config and terminal.
    """
    complete = getattr(llm, "complete", None)
    if not callable(complete):
        raise PipelineError("this Hermes gives plugins no model access (ctx.llm), cannot summarize",
                            step="summarize", code="model_unavailable", retryable=False)
    result = complete(
        [{"role": "system", "content": SUMMARY_SYSTEM},
         {"role": "user", "content": transcript}],
        timeout=SUMMARIZE_TIMEOUT_S, purpose="voice note summary")
    text = str(getattr(result, "text", "") or "").strip()
    if text.startswith("```"):  # a model that fences its JSON still meant it
        text = re.sub(r"^```[A-Za-z]*\s*", "", text)
        text = re.sub(r"\s*```$", "", text).strip()
    try:
        payload = json.loads(text)
    except ValueError:
        payload = None
    if not isinstance(payload, dict):
        raise PipelineError("the model did not answer with a JSON object", step="summarize",
                            code="unparseable_output", retryable=True)
    title, summary = payload.get("title"), payload.get("summary")
    points = payload.get("key_points") or []
    if not isinstance(title, str) or not title.strip() or not isinstance(summary, str) \
            or not summary.strip() or not isinstance(points, (list, tuple)):
        raise PipelineError("the model's JSON lacks a title, a summary or key_points",
                            step="summarize", code="unparseable_output", retryable=True)
    return Summary(title=_clean_title(title), summary=summary.strip(),
                   key_points=tuple(str(p).strip() for p in points if str(p).strip()))


# -- note metadata and the Obsidian note ------------------------------------------------

@dataclass(frozen=True)
class Summary:
    """What the model wrote about a transcript: the note's title, summary and
    key points (§5.6)."""

    title: str
    summary: str
    key_points: tuple[str, ...] = ()


@dataclass(frozen=True)
class NoteMeta:
    """The job's identity as staged in ``note.json`` (§5.2)."""

    job_id: str
    device_id: str
    recorded_at_utc: str  # YYYY-MM-DDTHH:MM:SSZ — keys the filename and the dedupe
    duration_seconds: int
    audio_sha256: str
    codec: str  # "wav" | "opus"
    attempts: int = 0  # failed attempts so far (§5.2); 0 means "process now"

    @classmethod
    def from_note_json(cls, entry: Path) -> "NoteMeta":
        path = entry / "note.json"
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise PipelineError(f"note.json is missing or unreadable: {exc}", step="verify",
                                code="invalid_metadata", retryable=False) from exc
        if not isinstance(raw, dict):
            raise PipelineError("note.json does not hold an object", step="verify",
                                code="invalid_metadata", retryable=False)

        def required(name: str) -> Any:
            if name not in raw:
                raise PipelineError(f"note.json lacks {name}", step="verify",
                                    code="invalid_metadata", retryable=False)
            return raw[name]

        try:
            job_id = str(required("job_id"))
            device_id = str(required("device_id"))
            duration = int(required("duration_seconds"))
            audio_sha256 = str(required("audio_sha256"))
            codec = str(required("codec")).lower()
        except (TypeError, ValueError) as exc:
            raise PipelineError(f"note.json holds a malformed field: {exc}", step="verify",
                                code="invalid_metadata", retryable=False) from exc
        if not _HEX64.fullmatch(job_id):
            raise PipelineError("job_id is not 64 hex characters", step="verify",
                                code="invalid_metadata", retryable=False)
        if not _DEVICE_ID.fullmatch(device_id):
            raise PipelineError("device_id is not filename-safe", step="verify",
                                code="invalid_metadata", retryable=False)
        if not 1 <= duration <= 900 or not _HEX64.fullmatch(audio_sha256) \
                or codec not in ("wav", "opus"):
            raise PipelineError("duration_seconds/audio_sha256/codec fail the contract's format",
                                step="verify", code="invalid_metadata", retryable=False)
        try:
            attempts = max(0, int(raw.get("attempts", 0)))
        except (TypeError, ValueError):
            attempts = 0
        return cls(job_id=job_id, device_id=device_id,
                   recorded_at_utc=_recorded_at_utc(raw),
                   duration_seconds=duration, audio_sha256=audio_sha256, codec=codec,
                   attempts=attempts)


def _recorded_at_utc(raw: Mapping[str, Any]) -> str:
    """The normalized ``recorded_at``: the stored UTC stamp, else the raw
    string (§5.2 stores both) normalized the way the endpoint does."""
    stored = str(raw.get("recorded_at_utc") or "")
    if stored:
        if not _UTC_STAMP.fullmatch(stored):
            raise PipelineError(f"recorded_at_utc {stored!r} is not YYYY-MM-DDTHH:MM:SSZ",
                                step="verify", code="invalid_metadata", retryable=False)
        return stored
    text = str(raw.get("recorded_at") or "").strip()
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError as exc:
        raise PipelineError(f"recorded_at {text!r} is not an ISO-8601 timestamp with an offset",
                            step="verify", code="invalid_metadata", retryable=False) from exc
    if parsed.tzinfo is None:
        raise PipelineError("recorded_at has no offset", step="verify",
                            code="invalid_metadata", retryable=False)
    return parsed.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _clean_title(value: str) -> str:
    """A single line of at most ``TITLE_MAX`` characters (§5.6)."""
    text = " ".join(str(value).split())
    if len(text) <= TITLE_MAX:
        return text
    cut = text[:TITLE_MAX].rstrip()
    if " " in cut:
        cut = cut.rsplit(" ", 1)[0].rstrip()
    return cut or text[:TITLE_MAX]


def _derive_title(summary: str) -> str:
    """A title from the summary itself, for a summarize step that returns only
    text (the model path writes its own title)."""
    text = str(summary).strip()
    return _clean_title(text.splitlines()[0]) if text else ""


def _coerce_summary(result: Any) -> Summary:
    """The injected summarize step's answer as a ``Summary``: the model returns
    one, a mapping or a bare string is accepted (older doubles), and anything
    else — or an empty summary — is unparseable model output (§5.8)."""
    if isinstance(result, Summary):
        summary, points, title = result.summary, result.key_points, result.title
    elif isinstance(result, Mapping):
        summary, points, title = result.get("summary"), result.get("key_points") or (), result.get("title")
    elif isinstance(result, str):
        summary, points, title = result, (), None
    else:
        raise PipelineError("the summarize step returned no usable summary", step="summarize",
                            code="unparseable_output", retryable=True)
    summary = str(summary or "").strip()
    if not summary:
        raise PipelineError("the summarize step returned an empty summary", step="summarize",
                            code="unparseable_output", retryable=True)
    if isinstance(points, str):
        points = (points,) if points.strip() else ()
    elif not isinstance(points, (list, tuple)):
        raise PipelineError("key_points is not a list", step="summarize",
                            code="unparseable_output", retryable=True)
    points = tuple(str(p).strip() for p in points if str(p).strip())
    title = str(title or "").strip() or _derive_title(summary)
    return Summary(title=_clean_title(title) or _derive_title(summary),
                   summary=summary, key_points=points)


def note_filename(meta: NoteMeta) -> str:
    """``2026-11-20_0815_hg-0123456789abcdef_3f9a1c7d.md`` — ``recorded_at``
    converted to the host's local timezone (DST-aware), the device id as sent,
    and ``job_id[:8]``, so a retry rebuilds the identical name (§5.5)."""
    when = datetime.strptime(meta.recorded_at_utc, "%Y-%m-%dT%H:%M:%SZ") \
        .replace(tzinfo=timezone.utc).astimezone()
    return f"{when:%Y-%m-%d}_{when:%H%M}_{meta.device_id}_{meta.job_id[:8]}.md"


def _yaml_scalar(value: object) -> str:
    """A frontmatter value: bare when YAML-safe, double-quoted otherwise."""
    text = str(value)
    if text and _PLAIN_SAFE.fullmatch(text) and ": " not in text and not text.endswith(":") \
            and " #" not in text and text == text.strip():
        return text
    return json.dumps(text, ensure_ascii=False)


def _block(text: str) -> list[str]:
    """The indented lines of a ``|`` frontmatter block scalar."""
    return ["  " + line if line.strip() else "" for line in str(text).split("\n")]


def render_note(meta: NoteMeta, summary: Summary, transcript: str, audio_name: str) -> str:
    """The markdown file: the fixed frontmatter keys in contract order (§5.6),
    then the body a reader sees. The transcript is never rewritten — it keeps
    exactly what STT returned; values are quoted only when YAML needs it."""
    lines = ["---",
             f"idempotency_key: {_yaml_scalar(meta.job_id)}",
             f"device_id: {_yaml_scalar(meta.device_id)}",
             f"recorded_at: {_yaml_scalar(meta.recorded_at_utc)}",
             f"duration_seconds: {meta.duration_seconds}",
             f"codec: {_yaml_scalar(meta.codec)}",
             f"audio_sha256: {_yaml_scalar(meta.audio_sha256)}",
             f"audio: {_yaml_scalar(f'{AUDIO_DIRNAME}/{audio_name}')}",
             f"title: {_yaml_scalar(summary.title)}",
             "transcript: |"]
    lines += _block(transcript)
    lines.append("summary: |")
    lines += _block(summary.summary)
    if summary.key_points:
        lines.append("key_points:")
        lines += [f"  - {_yaml_scalar(point)}" for point in summary.key_points]
    else:
        lines.append("key_points: []")
    lines.append("tags: [voice-note]")
    lines += ["---", "", f"# {summary.title}", "",
              "## Summary", "", summary.summary, "",
              "## Key points", ""]
    lines += [f"- {point}" for point in summary.key_points]
    if summary.key_points:
        lines.append("")
    lines += ["## Transcript", "", transcript, ""]
    return "\n".join(lines)


def _frontmatter(path: Path) -> Optional[dict[str, str]]:
    """The ``key: value`` lines of a note's frontmatter, or None when the file
    has no readable frontmatter. Block content never matches: it is indented,
    and only a line that is exactly ``---`` at column 0 closes the block."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            if handle.readline().rstrip("\r\n") != "---":
                return None
            lines: list[str] = []
            read = 0
            for line in handle:
                if line.rstrip("\r\n") == "---":
                    break
                lines.append(line.rstrip("\r\n"))
                read += len(line)
                if read > _FRONTMATTER_MAX_BYTES:  # corrupt or absurd: not a note we can scan
                    return None
            else:
                return None  # no closing delimiter
    except OSError:
        return None
    fields: dict[str, str] = {}
    for line in lines:
        found = _FM_FIELD.match(line)
        if found:
            fields[found.group(1)] = _unquote(found.group(2) or "")
    return fields


def _unquote(value: str) -> str:
    text = value.strip()
    if len(text) >= 2 and text[0] == text[-1] == '"':
        try:
            return json.loads(text)
        except ValueError:
            return text[1:-1]
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1]
    return text


# -- the pipeline ----------------------------------------------------------------------

class NotesPipeline:
    """Inbox to Obsidian: verify, transcribe, summarize, write, name, retry.

    ``transcribe`` is ``(audio_path, meta) -> str`` and ``summarize`` is
    ``(transcript) -> str | dict | Summary``; the defaults are the Hermes-backed
    steps above. ``llm_resolver`` returns this profile's ``ctx.llm`` (the
    adapter passes its ``_plugin_llm``), resolved per attempt so a gateway
    restart between attempts picks up the new one.
    """

    def __init__(self, notes_dir: Path | str, obsidian_dir: Path | str, *,
                 transcribe: Optional[Callable[..., str]] = None,
                 summarize: Optional[Callable[[str], Any]] = None,
                 language: Optional[str] = None,
                 llm_resolver: Optional[Callable[[], object]] = None,
                 clock: Callable[[], float] = time.time):
        self.notes_dir = Path(notes_dir)
        self.inbox = self.notes_dir / "inbox"
        self.done = self.notes_dir / "done"
        self.failed = self.notes_dir / "failed"
        self.obsidian_dir = Path(obsidian_dir).expanduser()
        self._transcribe = transcribe
        self._summarize = summarize
        self.language = language
        self._llm_resolver = llm_resolver
        self.clock = clock
        #: In-memory retry schedule (job_id -> unix time); the attempt count
        #: itself lives in note.json, so a restart retries promptly instead of
        #: waiting out a backoff that has not happened yet.
        self._due: dict[str, float] = {}

    # -- layout -----------------------------------------------------------------------

    def ensure_dirs(self) -> None:
        for directory in (self.inbox, self.done, self.failed):
            directory.mkdir(parents=True, exist_ok=True)
        (self.obsidian_dir / AUDIO_DIRNAME).mkdir(parents=True, exist_ok=True)

    # -- one pass ---------------------------------------------------------------------

    def run_once(self) -> Optional[float]:
        """Process every due inbox entry; return the seconds until the next entry
        is due, or None when nothing is waiting. Never raises: a pass failure is
        logged and retried by the caller's timer."""
        try:
            self.ensure_dirs()
        except OSError as exc:  # the vault's path may be unusable; entries still classify per job
            logger.warning("voice-note directories cannot be created: %s", exc)
        self._sweep_stale_tmp()
        try:
            entries = sorted(p for p in self.inbox.iterdir()
                             if p.is_dir() and not p.name.startswith("."))
        except OSError as exc:
            logger.warning("voice-note inbox unreadable: %s", exc)
            return None
        next_due: Optional[float] = None
        for entry in entries:
            due_at = self._due.get(entry.name)
            if due_at is None or due_at <= self.clock():  # None = not waiting; a time = backoff
                self._attempt(entry)
                due_at = self._due.get(entry.name)
            if due_at is not None:
                next_due = due_at if next_due is None else min(next_due, due_at)
        if next_due is None:
            return None
        return max(0.0, next_due - self.clock())

    def _sweep_stale_tmp(self) -> None:
        """Remove vault temp files older than 24 h — crash debris; a fresh one
        belongs to a write in flight (§5.7). Uses real time: the crash debris
        clock is the filesystem's, not the scheduler's."""
        cutoff = time.time() - TMP_MAX_AGE_S
        for directory in (self.obsidian_dir, self.obsidian_dir / AUDIO_DIRNAME):
            try:
                stale = [p for p in directory.glob("*.tmp-*") if p.is_file()]
            except OSError:
                return
            for path in stale:
                try:
                    if path.stat().st_mtime <= cutoff:
                        path.unlink()
                except OSError:  # best effort: a file that resists is swept next pass
                    pass

    def _attempt(self, entry: Path) -> None:
        job_id = entry.name
        try:
            meta = NoteMeta.from_note_json(entry)
        except Exception as exc:  # noqa: BLE001 - one bad entry must not kill the worker
            self._record_failure(entry, job_id, 1, classify(exc, "verify"))
            return
        if meta.attempts >= MAX_ATTEMPTS:  # a quarantine whose final move failed last pass
            self._re_quarantine(entry, meta)
            return
        attempts = meta.attempts
        step = "verify"
        try:
            if self._dedupe_hit(meta) is not None:
                logger.info("voice note %s already has an Obsidian note; closing the entry", meta.job_id)
                self._finish(entry)
                return
            audio = entry / f"recording.{meta.codec}"
            if not audio.is_file():
                raise PipelineError(f"recording.{meta.codec} is missing from the entry", step=step,
                                    code="missing_audio", retryable=False)
            digest = _sha256(audio)
            if digest != meta.audio_sha256:
                raise PipelineError(f"staged audio is {digest}, expected {meta.audio_sha256}",
                                    step=step, code="checksum_mismatch", retryable=False)
            step = "stt"
            transcript = str((self._transcribe or self._default_transcribe)(audio, meta) or "").strip()
            if not transcript:
                raise PipelineError("speech-to-text produced no text", step=step,
                                    code="empty_transcript", retryable=False)
            step = "summarize"
            summary = _coerce_summary((self._summarize or self._default_summarize)(transcript))
            step = "write"
            self._write_note(meta, summary, transcript, audio)
            self._finish(entry)
            logger.info("voice note %s written to %s", meta.job_id, self.obsidian_dir)
        except Exception as exc:  # noqa: BLE001 - one bad note must not kill the worker
            self._record_failure(entry, job_id, min(attempts + 1, MAX_ATTEMPTS),
                                 classify(exc, step))

    def _default_transcribe(self, audio: Path, meta: NoteMeta) -> str:
        return hermes_transcribe(audio, self.language)

    def _default_summarize(self, transcript: str) -> Summary:
        resolver = self._llm_resolver or (lambda: None)
        return hermes_summarize(resolver(), transcript)

    # -- attempt bookkeeping ----------------------------------------------------------

    def _record_failure(self, entry: Path, job_id: str, attempts: int,
                        error: PipelineError) -> None:
        """Record one failed attempt: quarantine on a terminal error or the
        attempt cap, otherwise count it and schedule the backoff retry."""
        exhausted = error.retryable and attempts >= MAX_ATTEMPTS
        if not error.retryable or exhausted:
            code = "attempts_exhausted" if exhausted else error.code
            self._quarantine(entry, job_id, code, error.step or "verify", str(error), attempts)
            return
        self._update_note_json(entry, attempts=attempts, last_error=f"{error.code}: {error}")
        (entry / ".error").unlink(missing_ok=True)  # a re-enqueue's leftovers must not linger
        delay = RETRY_BACKOFF_S[attempts - 1]
        self._due[job_id] = self.clock() + delay
        logger.warning("voice note %s attempt %d/%d failed at %s (%s: %s); retrying in %ds",
                       job_id, attempts, MAX_ATTEMPTS, error.step, error.code, error, delay)

    def _quarantine(self, entry: Path, job_id: str, code: str, step: str,
                    message: str, attempts: int) -> None:
        """Move the entry to ``failed/`` with ``.error`` naming the failure (§5.8)."""
        logger.error("voice note %s moved to failed/: %s (%s at %s, attempt %d)",
                     job_id, message, code, step, attempts)
        self._due.pop(job_id, None)
        self._update_note_json(entry, attempts=attempts, last_error=f"{code}: {message}")
        try:
            (entry / ".error").write_text(
                json.dumps({"code": code, "step": step, "message": message, "attempts": attempts},
                           indent=2, sort_keys=True) + "\n", encoding="utf-8")
            self._move(entry, self.failed)
        except OSError as exc:  # the next pass retries the move; the job is already accounted for
            logger.error("voice note %s could not be moved to failed/: %s", job_id, exc)

    def _re_quarantine(self, entry: Path, meta: NoteMeta) -> None:
        """Re-attempt the move of an already-quarantined job, keeping the
        ``.error`` the original failure wrote."""
        code, step = "attempts_exhausted", "verify"
        message = f"failed after {meta.attempts} attempts"
        try:
            previous = json.loads((entry / ".error").read_text(encoding="utf-8"))
            code = str(previous.get("code") or code)
            step = str(previous.get("step") or step)
            message = str(previous.get("message") or message)
        except (OSError, ValueError):
            pass
        self._quarantine(entry, meta.job_id, code, step, message, meta.attempts)

    def _finish(self, entry: Path) -> None:
        """The note exists (or already did): close the job (§5.7 step 5)."""
        for name in (".error", ".attempts", ".next_retry"):  # the last two: entries staged by the prototype
            (entry / name).unlink(missing_ok=True)
        self._due.pop(entry.name, None)
        self._move(entry, self.done)

    def _move(self, entry: Path, target_dir: Path) -> None:
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / entry.name
        if target.exists():  # both dirs hold the id: the newer one is a re-upload; keep the first
            shutil.rmtree(entry, ignore_errors=True)
            return
        os.replace(entry, target)

    def _update_note_json(self, entry: Path, **fields: Any) -> None:
        """Record attempt bookkeeping on the entry (§5.2), atomically and best
        effort — failing to record must not mask the failure being recorded."""
        path = entry / "note.json"
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            raw.update(fields)
            tmp = entry / "note.json.tmp"
            with tmp.open("w", encoding="utf-8") as handle:
                json.dump(raw, handle, indent=2, sort_keys=True)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp, path)
        except (OSError, ValueError, AttributeError) as exc:
            logger.warning("cannot record attempt bookkeeping in %s: %s", path, exc)
            try:
                (entry / "note.json.tmp").unlink(missing_ok=True)
            except OSError:
                pass

    # -- the Obsidian side ------------------------------------------------------------

    def _dedupe_hit(self, meta: NoteMeta) -> Optional[Path]:
        """A note already recording this job, or this recording under another
        key (§5.7 step 1): the frontmatter is the dedupe key, so two job ids
        carrying one recording still make one note (§5.3)."""
        try:
            candidates = sorted(self.obsidian_dir.glob("*.md"))
        except OSError:
            return None
        for note in candidates:
            fields = _frontmatter(note)
            if not fields:
                continue
            if fields.get("idempotency_key") == meta.job_id:
                return note
            if fields.get("device_id") == meta.device_id \
                    and fields.get("recorded_at") == meta.recorded_at_utc \
                    and fields.get("audio_sha256") == meta.audio_sha256:
                return note
        return None

    def _write_note(self, meta: NoteMeta, summary: Summary, transcript: str,
                    audio: Path) -> None:
        """Audio first, note last, each written to a temp file and renamed
        (§5.7): the markdown replace is the commit point, so a failure before
        it leaves no ``.md``, and an orphaned audio copy is harmless and gets
        replaced idempotently on the next attempt."""
        name = note_filename(meta)
        stem = os.path.splitext(name)[0]
        target = self.obsidian_dir / name
        taken = 1
        while target.exists():  # a 32-bit short-id clash: suffix, never overwrite (§5.5)
            taken += 1
            target = self.obsidian_dir / f"{stem}-{taken}.md"
        audio_name = f"{os.path.splitext(target.name)[0]}.{meta.codec}"
        audio_dir = self.obsidian_dir / AUDIO_DIRNAME
        audio_dir.mkdir(parents=True, exist_ok=True)
        short = meta.job_id[:8]
        tmp_audio = audio_dir / f"{audio_name}.tmp-{short}"
        tmp_note = target.with_name(f"{target.name}.tmp-{short}")
        try:
            _copy_fsync(audio, tmp_audio)
            os.replace(tmp_audio, audio_dir / audio_name)
            _write_fsync(tmp_note, render_note(meta, summary, transcript, audio_name))
            os.replace(tmp_note, target)
        except BaseException:
            tmp_audio.unlink(missing_ok=True)
            tmp_note.unlink(missing_ok=True)
            raise
        _fsync_dir(self.obsidian_dir)  # best effort: filesystems that refuse are fine


# -- small helpers ---------------------------------------------------------------------

def _sha256(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _copy_fsync(src: Path, dst: Path) -> None:
    with src.open("rb") as reader, dst.open("wb") as writer:
        shutil.copyfileobj(reader, writer)
        writer.flush()
        os.fsync(writer.fileno())


def _write_fsync(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())


def _fsync_dir(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)
