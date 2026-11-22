"""The voice-note upload endpoint: ``POST /notes/v1`` on its own port (default 8766).

Normative contract: ``docs/voice-note-ingestion.md``. A device that recorded
offline sends **one multipart/form-data request** whose ``audio`` file part is
the whole recording and whose text parts are the metadata it persisted beside
the audio (``device_id``, ``recorded_at``, ``duration_seconds``, and optionally
``idempotency_key`` and ``checksum``). ``X-Gadget-Ts``/``X-Gadget-Mac`` (and
``X-Gadget-Token`` when configured) authorize the request; the MAC covers the
field values exactly as transmitted plus the SHA-256 of the audio bytes.

The request is handled in the contract's fixed order (§4), and each answer is
the *first* failing check::

    404/405  path/method
    400      content type, then multipart parse (missing/repeated/non-file audio)
    413      request or audio over the cap
    422      field format/range
    415      audio bytes are not WAV PCM16 mono or Ogg Opus mono
    401      auth (device key, ts skew, MAC, token)
    409      supplied ``checksum`` does not match the received bytes
    200/409/202  duplicate / idempotency conflict / accepted
    507/503  storage failed / server paused
    500      unexpected error (the only answer nothing maps to; retryable)

Validation deliberately precedes authentication, so a malformed request is
answered without consulting the device store, and contract tests for the
validation steps need no device key.

202 is sent as soon as the entry is durably staged under
``notes/inbox/<job_id>/`` (``recording.<codec>`` + ``note.json``), before any
transcription: the device never waits on a model. ``notes_pipeline`` owns the
rest (STT, summary, Obsidian write, quarantine) and is woken here, not called
inline.

One server per gateway process even when several profiles each load their own
copy of the plugin (``retain``/``release`` plus the process-wide ``runtime``
registry): the port is a device-facing endpoint, not a per-profile one.
"""

from __future__ import annotations

import asyncio
import errno
import hashlib
import hmac
import json
import logging
import mmap
import os
import re
import shutil
import tempfile
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Optional

from . import protocol

logger = logging.getLogger(__name__)

try:  # the pipeline's exports are stable; import lazily-safe for a stripped test env
    from .notes_pipeline import IDLE_POLL_S, NotesPipeline
except Exception:  # pragma: no cover - the pipeline ships beside this module
    IDLE_POLL_S = 120.0
    NotesPipeline = None  # type: ignore[assignment]

NOTE_PATH = "/notes/v1"
HEALTH_PATH = "/health"

#: The contract's audio cap (§2.4): 2 GiB, the same ceiling the device records
#: to (protocol.NOTE_MAX_BYTES), so a device-accepted note is never refused here.
MAX_NOTE_BYTES = protocol.NOTE_MAX_BYTES
#: The whole request may carry the audio plus 64 KiB of metadata and boundaries.
MAX_REQUEST_BYTES = MAX_NOTE_BYTES + 64 * 1024
MAX_SKEW_S = 300
DURATION_MIN = protocol.NOTE_DURATION_MIN
DURATION_MAX = protocol.NOTE_DURATION_MAX

#: Transient staging write errors are retried three times, 100 ms apart (§5.7),
#: inside the request; anything else (notably ``ENOSPC``) answers 507 at once and
#: lets the device's own backoff handle it.
STAGING_RETRIES = 3
STAGING_RETRY_DELAY_S = 0.1
_TRANSIENT_ERRNOS = frozenset({errno.EINTR, errno.EAGAIN, errno.EBUSY})

H_TS = "x-gadget-ts"
H_MAC = "x-gadget-mac"
H_TOKEN = "x-gadget-token"

DEFAULT_OBSIDIAN_DIR = "~/obsidian/voice notes"

_AUDIO = "audio"
_DEVICE = "device_id"
_RECORDED_AT = "recorded_at"
_DURATION = "duration_seconds"
_IDEMPOTENCY = "idempotency_key"
_CHECKSUM = "checksum"
_REQUIRED = (_AUDIO, _DEVICE, _RECORDED_AT, _DURATION)
_TEXT_PARTS = frozenset((_DEVICE, _RECORDED_AT, _DURATION, _IDEMPOTENCY, _CHECKSUM))
_KNOWN_PARTS = frozenset((_AUDIO,) + tuple(_TEXT_PARTS))

_DEVICE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_IDEMPOTENCY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_DECIMAL = re.compile(r"^[0-9]+$")
_TS = re.compile(r"^[0-9]+$")
#: ISO-8601 with an offset: optional fractional seconds, ``Z``/``z`` or ±hh:mm / ±hhmm.
_RECORDED_AT_RE = re.compile(
    r"^(?P<y>\d{4})-(?P<mo>\d{2})-(?P<d>\d{2})[Tt]"
    r"(?P<h>\d{2}):(?P<mi>\d{2}):(?P<s>\d{2})(?P<frac>\.\d{1,6})?"
    r"(?P<off>[Zz]|[+-]\d{2}:?\d{2})$")


# -- response envelope -----------------------------------------------------------------


def _fail(status: int, code: str, message: str, *, field: Optional[str] = None,
          retryable: bool = False) -> tuple[int, dict]:
    """The contract's error envelope (§3.2): ``field`` only when one part is at fault."""
    error: dict[str, Any] = {"code": code, "message": message}
    if field is not None:
        error["field"] = field
    error["retryable"] = retryable
    return status, {"error": error}


class _Malformed(ValueError):
    """The multipart body cannot be parsed; answered as 400 (§3.2)."""

    def __init__(self, message: str, field: Optional[str] = None):
        super().__init__(message)
        self.field = field


class _Rejected(Exception):
    """Carries an already-shaped ``(status, payload)`` answer out of a helper."""

    def __init__(self, status: int, payload: dict):
        super().__init__(payload["error"]["message"])
        self.status = status
        self.payload = payload


class _Inconsistent(Exception):
    """A stored entry exists but its note.json cannot be read (§5.3 edge)."""


# -- multipart parsing -----------------------------------------------------------------


@dataclass(frozen=True)
class _Part:
    name: str
    filename: Optional[str]
    #: A zero-copy view into the request body (see parse_multipart).
    value: bytes | memoryview
    content_type: Optional[str]

    @property
    def is_file(self) -> bool:
        return self.filename is not None


def _content_type(header: Optional[str]) -> tuple[str, dict[str, str]]:
    """Split a ``Content-Type`` header into its media type and parameters."""
    if not header:
        return "", {}
    pieces = _split_parameters(str(header))
    return pieces[0].strip().lower(), pieces[1]


def _split_parameters(value: str) -> tuple[str, dict[str, str]]:
    """``a; b="x;y"; c=z`` -> (``a``, {``b``: ``x;y``, ``c``: ``z``})."""
    segments: list[str] = []
    buf: list[str] = []
    quoted = False
    for char in value:
        if char == '"':
            quoted = not quoted
            buf.append(char)
        elif char == ";" and not quoted:
            segments.append("".join(buf))
            buf = []
        else:
            buf.append(char)
    segments.append("".join(buf))
    params: dict[str, str] = {}
    for segment in segments[1:]:
        if "=" not in segment:
            continue
        key, _, raw = segment.partition("=")
        raw = raw.strip()
        if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
            raw = raw[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        params[key.strip().lower()] = raw
    return segments[0], params


def _part_headers(block: bytes) -> dict[str, str]:
    headers: dict[str, str] = {}
    for line in block.split(b"\r\n"):
        name, sep, value = line.partition(b":")
        if not sep:
            continue
        headers[name.strip().decode("latin-1").lower()] = value.strip().decode("latin-1")
    return headers


def parse_multipart(body, boundary: str) -> list[_Part]:
    """Parse a ``multipart/form-data`` body into its parts.

    Deliberately a small, strict parser over the fully-read body: doing it here
    (instead of a framework form parser) keeps the contract's exact-value and
    repeated-part rules under our control. ``body`` is anything a route may hand
    over — ``bytes`` (the direct entry point tests drive), a ``bytearray``, or an
    ``mmap`` of the spooled request — so only ``body.find`` is required; every
    part's value is a zero-copy ``memoryview`` into it, which is what keeps a
    multi-gigabyte note from being copied twice before it is written out.
    Raises ``_Malformed`` for anything that is not a well-formed multipart body.
    """
    if not boundary:
        raise _Malformed("the Content-Type has no multipart boundary")
    delimiter = b"--" + boundary.encode("latin-1")
    view = memoryview(body)
    start = body.find(delimiter)
    if start < 0:
        raise _Malformed("the body is not multipart: its boundary was not found")
    pos = start + len(delimiter)
    parts: list[_Part] = []
    while True:
        if view[pos:pos + 2] == b"--":  # closing delimiter, epilogue ignored
            return parts
        if view[pos:pos + 2] == b"\r\n":
            pos += 2
        elif view[pos:pos + 1] == b"\n":  # tolerate a bare LF after the boundary
            pos += 1
        else:
            raise _Malformed("malformed multipart: a boundary is not followed by CRLF")
        head_end = body.find(b"\r\n\r\n", pos)
        if head_end < 0:
            raise _Malformed("malformed multipart: a part's headers never end")
        headers = _part_headers(bytes(view[pos:head_end]))
        start_of_value = head_end + 4
        end = body.find(b"\r\n" + delimiter, start_of_value)
        if end < 0:
            raise _Malformed("malformed multipart: a part is not closed by the boundary")
        value = view[start_of_value:end]
        pos = end + 2 + len(delimiter)
        disposition = headers.get("content-disposition", "")
        kind, params = _split_parameters(disposition)
        name = params.get("name")
        if kind.strip().lower() != "form-data" or not name:
            raise _Malformed("malformed multipart: a part has no form-data name")
        parts.append(_Part(name=name, filename=params.get("filename"), value=value,
                           content_type=headers.get("content-type")))


# -- field validation ------------------------------------------------------------------


def normalize_recorded_at(raw: str) -> str:
    """``recorded_at`` -> ``YYYY-MM-DDTHH:MM:SSZ``; ValueError when it is not
    ISO-8601 with an offset (§2). The raw string is what the MAC covers and what
    ``note.json`` keeps, so this only produces the normalized form."""
    match = _RECORDED_AT_RE.match(raw)
    if match is None:
        raise ValueError("recorded_at must be ISO-8601 with an offset, e.g. 2026-11-20T08:15:04-06:00")
    off = match.group("off")
    if off in ("Z", "z"):
        tz = timezone.utc
    else:
        sign = 1 if off[0] == "+" else -1
        digits = off[1:].replace(":", "")
        tz = timezone(sign * timedelta(hours=int(digits[:2]), minutes=int(digits[2:])))
    frac = match.group("frac") or ""
    micro = int((frac[1:] + "000000")[:6]) if frac else 0
    try:
        moment = datetime(int(match.group("y")), int(match.group("mo")), int(match.group("d")),
                          int(match.group("h")), int(match.group("mi")), int(match.group("s")),
                          micro, tzinfo=tz)
    except ValueError as exc:
        raise ValueError(f"recorded_at is not a real timestamp: {exc}") from exc
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sniff_codec(audio: bytes | memoryview) -> Optional[str]:
    """``"wav"``/``"opus"`` for audio the contract accepts, else None (§2.3).

    WAV: ``RIFF``…``WAVE`` with a PCM (format 1) 16-bit mono ``fmt `` chunk at
    8-48 kHz. Opus: an Ogg container whose first packet is ``OpusHead`` declaring
    one channel. The declared part type and filename are ignored on purpose.
    """
    if len(audio) >= 12 and audio[:4] == b"RIFF" and audio[8:12] == b"WAVE":
        pos = 12
        while pos + 8 <= len(audio):
            chunk = audio[pos:pos + 4]
            size = int.from_bytes(audio[pos + 4:pos + 8], "little")
            body = audio[pos + 8:pos + 8 + size]
            if chunk == b"fmt ":
                if len(body) < 16:
                    return None
                fmt = int.from_bytes(body[0:2], "little")
                channels = int.from_bytes(body[2:4], "little")
                rate = int.from_bytes(body[4:8], "little")
                bits = int.from_bytes(body[14:16], "little")
                if fmt != 1 or channels != 1 or bits != 16 or not 8000 <= rate <= 48000:
                    return None
                return "wav"
            pos += 8 + size + (size & 1)  # chunks are word-aligned
        return None
    if audio[:4] == b"OggS":
        # Ogg's first page header is small: copying it is cheaper than teaching
        # every caller to carry a bytes-like that also has .find().
        head = bytes(audio[:512]).find(b"OpusHead")
        if head < 0 or head + 10 > len(audio):
            return None
        return "opus" if audio[head + 9] == 1 else None
    return None


@dataclass(frozen=True)
class _Request:
    """A parsed, field-validated upload — everything auth and storage need."""

    device_id: str
    recorded_at_raw: str  # exactly as transmitted: MAC input, kept in note.json
    recorded_at_utc: str  # normalized: job_id derivation, note.json
    duration_raw: str  # exactly as transmitted: MAC input
    duration_seconds: int
    audio: bytes | memoryview  # view into the body's buffer, never a copy
    codec: str
    idempotency_key: Optional[str]
    checksum: Optional[str]
    audio_sha256: str


# -- durable file helpers --------------------------------------------------------------


def _retry_transient(action: Callable[[], Any]) -> Any:
    """Run ``action``, retrying EINTR/EAGAIN/EBUSY three times 100 ms apart (§5.7)."""
    for attempt in range(STAGING_RETRIES + 1):
        try:
            return action()
        except OSError as exc:
            if exc.errno not in _TRANSIENT_ERRNOS or attempt == STAGING_RETRIES:
                raise
            time.sleep(STAGING_RETRY_DELAY_S)
    raise AssertionError("unreachable")


def _write_file(path: Path, data: bytes | bytearray | memoryview) -> None:
    """One write attempt: open, write, fsync (the caller adds the transient retry)."""
    with open(path, "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def _write_durable(path: Path, data: bytes | bytearray | memoryview) -> None:
    """Write and fsync one file, reopening and retrying the transient errors."""
    _retry_transient(lambda: _write_file(path, data))


def _write_atomic(path: Path, data: bytes) -> None:
    """Replace ``path`` with ``data`` through a sibling temp file (commit point)."""
    tmp = path.with_name(path.name + ".tmp")
    _write_durable(tmp, data)
    _retry_transient(lambda: os.replace(tmp, path))


def _fsync_dir(path: Path) -> None:
    """Best effort: a filesystem that refuses the fsync is not an error here."""
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


class NotesServer:
    """HTTP endpoint + pipeline worker for one gateway process.

    Not started by itself: ``start()`` brings up the worker thread and the
    uvicorn thread (or raises with the bind error, which the adapter logs — a
    busy port must not take the platform down). ``retain()``/``release()``
    reference-count the shared instance across profile copies of the plugin.
    """

    def __init__(self, store: Any, notes_dir: Path | str, *,
                 host: str = "0.0.0.0", port: int = 8766,
                 max_bytes: int = MAX_NOTE_BYTES,
                 access_token: Optional[str] = None,
                 obsidian_dir: Optional[Path | str] = None,
                 language: Optional[str] = None,
                 llm_resolver: Optional[Callable[[], object]] = None,
                 transcribe: Optional[Callable[..., str]] = None,
                 summarize: Optional[Callable[[str], str]] = None,
                 stage_only: bool = False,
                 page_dir: Optional[Path | str] = None,
                 clock: Callable[[], float] = time.time):
        self.store = store
        self.notes_dir = Path(notes_dir)
        self.inbox = self.notes_dir / "inbox"
        self.done = self.notes_dir / "done"
        self.failed = self.notes_dir / "failed"
        self._staging = self.notes_dir / ".staging"
        self.host = host
        self.port = port
        self.max_bytes = int(max_bytes)
        self.access_token = access_token or None
        vault = obsidian_dir if obsidian_dir is not None else DEFAULT_OBSIDIAN_DIR
        # ``stage_only`` is the container's mode without a transcriber: uploads are
        # verified and staged in ``inbox/`` exactly as the contract says, and the
        # host that mounts the same directory runs the pipeline.
        self.pipeline = (NotesPipeline(self.notes_dir, Path(vault).expanduser(),
                                       transcribe=transcribe, summarize=summarize,
                                       language=language, llm_resolver=llm_resolver, clock=clock)
                         if NotesPipeline is not None and not stage_only else None)
        #: Served at ``/`` from the same origin as ``/notes/v1`` (the phone's page).
        self.page_dir = Path(page_dir) if page_dir is not None else None
        self.paused = False  # maintenance: every upload gets 503 and the device retries
        self.bound_port: Optional[int] = None
        self.last_error: Optional[str] = None
        self._refs = 0
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._worker: Optional[threading.Thread] = None
        self._http: Optional[threading.Thread] = None
        self._uv_server: Any = None
        self._uv_failed = threading.Event()
        self._job_locks: dict[str, threading.Lock] = {}
        self._job_locks_guard = threading.Lock()

    # -- lifecycle -------------------------------------------------------------------

    def retain(self) -> "NotesServer":
        with self._lock:
            self._refs += 1
        return self

    def release(self) -> bool:
        """Drop one user; True when the last one let go and the server stopped."""
        with self._lock:
            self._refs = max(0, self._refs - 1)
            stop = self._refs == 0
        if stop and self._worker is not None:
            self.stop()
        return stop

    @property
    def staging_dir(self) -> Path:
        """Where a request body is spooled and an entry is built (``.staging``).

        The route spools here rather than in ``/tmp``: it is the same filesystem
        as ``inbox/``, so a note that does not fit fails there instead of being
        copied across mounts, and it is cleaned by the same rule.
        """
        return self._staging

    @property
    def refs(self) -> int:
        with self._lock:
            return self._refs

    def start(self) -> None:
        """Bring up the pipeline worker and the HTTP thread; raise if the port never binds."""
        if self.pipeline is not None:
            self.pipeline.ensure_dirs()
        for directory in (self.inbox, self.done, self.failed, self._staging):
            directory.mkdir(parents=True, exist_ok=True)
        self._stop.clear()
        self._worker = threading.Thread(target=self._work, name="hg-notes-pipeline", daemon=True)
        self._worker.start()
        self._http = threading.Thread(target=self._run_http, name="hg-notes-http", daemon=True)
        self._http.start()
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            server = self._uv_server
            if server is not None and server.started:
                self.bound_port = self._bound_port()
                logger.info("voice-note endpoint on http://%s:%s%s",
                            self.host, self.bound_port, NOTE_PATH)
                return
            if self._uv_failed.is_set():
                break
            time.sleep(0.02)
        self._stop_worker()
        http, self._http = self._http, None
        if http is not None:
            http.join(timeout=2.0)
        raise RuntimeError(self.last_error or f"cannot listen on {self.host}:{self.port}")

    def stop(self) -> None:
        # A shutdown answers 503 (§3.2) rather than half-serving an upload.
        self.paused = True
        self._stop_worker()
        server, self._uv_server = self._uv_server, None
        if server is not None:
            server.should_exit = True
        thread, self._http = self._http, None
        if thread is not None:
            thread.join(timeout=10.0)
        self.bound_port = None

    def _stop_worker(self) -> None:
        self._stop.set()
        self._wake.set()
        worker, self._worker = self._worker, None
        if worker is not None:
            worker.join(timeout=5.0)

    def notify(self) -> None:
        """Wake the pipeline: a note just landed (or a test wants a pass now).

        Waking cannot fail an upload — the entry is already durable and the idle
        poll picks it up even if this wake is lost (§5.7 step 3).
        """
        try:
            self._wake.set()
        except Exception:  # noqa: BLE001 - an upload must not fail on a wake
            logger.exception("voice-note wake failed")

    # -- the worker ------------------------------------------------------------------

    def _work(self) -> None:
        while not self._stop.is_set():
            delay: Optional[float] = None
            try:
                if self.pipeline is not None:
                    delay = self.pipeline.run_once()
            except Exception:  # noqa: BLE001 - a pass must never kill the worker
                logger.exception("voice-note pipeline pass failed")
                delay = 30.0
            self._wake.wait(timeout=IDLE_POLL_S if delay is None else delay)
            self._wake.clear()

    def _run_http(self) -> None:
        try:
            import uvicorn
        except ImportError as exc:  # both ship with Hermes; a stripped env says so loudly
            self.last_error = f"uvicorn is unavailable: {exc}"
            self._uv_failed.set()
            return
        try:
            config = uvicorn.Config(create_app(self, self.page_dir), host=self.host, port=self.port,
                                    log_config=None, access_log=False)
            server = uvicorn.Server(config)
            self._uv_server = server
            server.run()
            if not server.started:
                self.last_error = self.last_error or "the note endpoint stopped before it was listening"
                self._uv_failed.set()
        except BaseException as exc:  # noqa: BLE001 - bind errors arrive as SystemExit/OSError
            if isinstance(exc, SystemExit):
                self.last_error = (f"exited with {exc.code} before listening "
                                   f"(is port {self.port} on {self.host} free?)")
            else:
                self.last_error = f"{type(exc).__name__}: {exc}"
            logger.error("voice-note endpoint failed on %s:%s: %s", self.host, self.port, self.last_error)
            self._uv_failed.set()

    def _bound_port(self) -> Optional[int]:
        server = self._uv_server
        try:
            for sock in server.servers[0].sockets or []:
                return int(sock.getsockname()[1])
        except (AttributeError, IndexError, OSError):
            return None
        return None

    # -- request handling (sync, unit-testable without the HTTP layer) ---------------

    def precheck(self, headers: Mapping[str, str], content_type: Optional[str],
                 content_length: Optional[int], body_len: Optional[int] = None) -> Optional[tuple[int, dict]]:
        """Checks that need no body: paused, media type, request size (§4 steps 1-3).

        The route calls this before reading the body so an oversize or
        non-multipart request is answered without buffering it; ``handle_upload``
        calls it too, so the direct (test) entry point enforces the same order.
        """
        if self.paused:
            return _fail(503, "unavailable", "the voice-note endpoint is paused", retryable=True)
        media, params = _content_type(content_type)
        if media != "multipart/form-data" or not params.get("boundary"):
            return _fail(400, "bad_request",
                         "the request must be multipart/form-data with a boundary")
        if content_length is not None and content_length > MAX_REQUEST_BYTES:
            return _fail(413, "too_large", f"the request exceeds {MAX_REQUEST_BYTES} bytes")
        if body_len is not None and body_len > MAX_REQUEST_BYTES:
            return _fail(413, "too_large", f"the request exceeds {MAX_REQUEST_BYTES} bytes")
        return None

    def handle_upload(self, headers: Mapping[str, str], content_type: Optional[str],
                      content_length: Optional[int],
                      body: bytes | bytearray | mmap.mmap,
                      received_at: Optional[float] = None) -> tuple[int, dict]:
        """Validate, authorize, de-duplicate and stage one upload; answer as §3 says.

        This is the whole contract minus the byte-moving HTTP layer, which is why
        tests drive it directly. ``received_at`` is when the request's headers
        arrived (the route's clock); the skew window is measured from it, so a
        body that streams for longer than the window is not called outdated.
        """
        early = self.precheck(headers, content_type, content_length, len(body))
        if early is not None:
            return early
        try:
            request = self._validate(body, content_type)
        except _Rejected as rejection:
            return rejection.status, rejection.payload
        error = self._authorize(dict(headers), request, received_at)
        if error is not None:
            return error
        return self._accept(request)

    def _validate(self, body: bytes | bytearray | mmap.mmap, content_type: Optional[str]) -> _Request:
        """§4 steps 4-5 plus the codec check: parse, then formats and ranges."""
        _, params = _content_type(content_type)
        try:
            parts = parse_multipart(body, params.get("boundary", ""))
        except _Malformed as exc:
            raise _Rejected(*_fail(400, "bad_request", str(exc), field=exc.field))

        seen: set[str] = set()
        fields: dict[str, _Part] = {}
        files: dict[str, _Part] = {}
        for part in parts:
            if part.name in seen:
                raise _Rejected(*_fail(400, "bad_request",
                                       f"part {part.name!r} appears more than once", field=part.name))
            seen.add(part.name)
            if part.name not in _KNOWN_PARTS:
                continue  # unknown parts are ignored (§2)
            (files if part.is_file else fields)[part.name] = part

        for name in _REQUIRED:
            if name not in seen:
                raise _Rejected(*_fail(400, "bad_request", f"{name} is required", field=name))
        audio_part = files.get(_AUDIO)
        if audio_part is None:
            raise _Rejected(*_fail(400, "bad_request", "audio must be a file part", field=_AUDIO))
        for name in _TEXT_PARTS:
            if name in files:
                raise _Rejected(*_fail(400, "bad_request", f"{name} must be a text part", field=name))
        if not audio_part.value:
            raise _Rejected(*_fail(400, "bad_request", "audio is empty", field=_AUDIO))
        if len(audio_part.value) > self.max_bytes:
            raise _Rejected(*_fail(413, "too_large",
                                   f"audio exceeds {self.max_bytes} bytes", field=_AUDIO))

        def text(name: str) -> str:
            try:
                return bytes(fields[name].value).decode("utf-8")
            except UnicodeDecodeError as exc:
                raise _Rejected(*_fail(422, "invalid_field",
                                       f"{name} is not UTF-8 text", field=name)) from exc

        device_id = text(_DEVICE)
        if _DEVICE_RE.fullmatch(device_id) is None:
            raise _Rejected(*_fail(422, "invalid_field",
                                   "device_id must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}",
                                   field=_DEVICE))

        recorded_at_raw = text(_RECORDED_AT)
        try:
            recorded_at_utc = normalize_recorded_at(recorded_at_raw)
        except ValueError as exc:
            raise _Rejected(*_fail(422, "invalid_field", str(exc), field=_RECORDED_AT)) from exc
        moment = datetime.strptime(recorded_at_utc, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        if (moment - datetime.now(timezone.utc)).total_seconds() > MAX_SKEW_S:
            raise _Rejected(*_fail(422, "invalid_field",
                                   "recorded_at is more than 300 s in the future", field=_RECORDED_AT))

        duration_raw = text(_DURATION)
        if _DECIMAL.fullmatch(duration_raw) is None or not DURATION_MIN <= int(duration_raw) <= DURATION_MAX:
            raise _Rejected(*_fail(422, "invalid_field",
                                   f"duration_seconds must be an integer in {DURATION_MIN}..{DURATION_MAX}",
                                   field=_DURATION))

        idempotency_key = None
        if _IDEMPOTENCY in fields:
            idempotency_key = text(_IDEMPOTENCY)
            if _IDEMPOTENCY_RE.fullmatch(idempotency_key) is None:
                raise _Rejected(*_fail(422, "invalid_field",
                                       "idempotency_key must match [A-Za-z0-9][A-Za-z0-9._-]{0,127}",
                                       field=_IDEMPOTENCY))

        checksum = None
        if _CHECKSUM in fields:
            checksum = text(_CHECKSUM)
            if _HEX64.fullmatch(checksum) is None:
                raise _Rejected(*_fail(422, "invalid_field",
                                       "checksum must be 64 lowercase hex characters", field=_CHECKSUM))

        codec = sniff_codec(audio_part.value)
        if codec is None:
            raise _Rejected(*_fail(415, "unsupported_codec",
                                   "audio must be WAV PCM16 mono (8-48 kHz) or Ogg Opus mono",
                                   field=_AUDIO))
        return _Request(device_id=device_id, recorded_at_raw=recorded_at_raw,
                        recorded_at_utc=recorded_at_utc, duration_raw=duration_raw,
                        duration_seconds=int(duration_raw), audio=audio_part.value, codec=codec,
                        idempotency_key=idempotency_key, checksum=checksum,
                        audio_sha256=hashlib.sha256(audio_part.value).hexdigest())

    def _authorize(self, headers: Mapping[str, str], request: _Request,
                   received_at: Optional[float] = None) -> Optional[tuple[int, dict]]:
        """§4 step 6: timestamp skew, enrollment, MAC, optional access token.

        The MAC covers the raw field strings and the digest of the bytes we
        actually received, so a truncated or mangled transfer fails here as well
        as at the checksum step.

        ``received_at`` is when the request's headers arrived, and the clock
        window is measured from there rather than from now: the body is spooled
        before this runs, and a note may take far longer to stream than the
        window is wide (248 MB is ~17 min on the board). The device stamps
        ``X-Gadget-Ts`` once, before its first byte, and cannot refresh it
        afterwards because the MAC covers it, so measuring after the body would
        reject every note that is large or slow rather than outdated
        (t_1ea185ed).
        """
        h = {str(k).lower(): str(v) for k, v in headers.items()}
        ts_raw = h.get(H_TS, "")
        mac = h.get(H_MAC, "")
        if _TS.fullmatch(ts_raw) is None:
            return _fail(401, "auth", "X-Gadget-Ts must be decimal unix seconds")
        now = time.time() if received_at is None else received_at
        if abs(now - int(ts_raw)) > MAX_SKEW_S:
            return _fail(401, "auth", "X-Gadget-Ts is outside the ±300 s window")
        key = self.store.key_for(request.device_id) if self.store is not None else None
        if not key:
            return _fail(401, "auth", f"device {request.device_id} is not enrolled")
        if not protocol.verify_note_mac(key, request.device_id, ts_raw, request.recorded_at_raw,
                                        request.duration_raw, request.audio_sha256,
                                        request.idempotency_key, mac):
            return _fail(401, "auth", "X-Gadget-Mac does not match the request")
        if self.access_token is not None:
            token = h.get(H_TOKEN, "")
            if not hmac.compare_digest(token.encode(), self.access_token.encode()):
                return _fail(401, "auth", "X-Gadget-Token does not match")
        return None

    def _accept(self, request: _Request) -> tuple[int, dict]:
        """§4 steps 7-10: checksum, idempotency matrix, durable staging, 202."""
        if request.checksum is not None and request.checksum != request.audio_sha256:
            return _fail(409, "checksum_mismatch",
                         "the checksum part does not match the received audio")
        job_id = self.job_id_for(request)
        lock = self._job_lock(job_id)
        try:
            try:
                existing = self._existing(job_id)
            except _Inconsistent as exc:
                logger.error("voice-note job %s is unreadable: %s", job_id, exc)
                return _fail(500, "internal", str(exc), retryable=True)
            if existing is not None:
                state, stored_sha = existing
                if stored_sha != request.audio_sha256:
                    logger.error("voice note %s re-uploaded with different audio", job_id)
                    return _fail(409, "idempotency_conflict",
                                 "this job_id was already stored with different audio")
                if state == "failed":
                    return self._reopen(request, job_id)
                logger.info("voice note %s already stored (%s); answering duplicate", job_id, state)
                self.notify()
                return 200, {"status": "duplicate", "job_id": job_id}
            try:
                self._store(job_id, request)
            except FileExistsError:  # lost the rename race: the winner's entry is authoritative
                try:
                    existing = self._existing(job_id)
                except _Inconsistent as exc:
                    return _fail(500, "internal", str(exc), retryable=True)
                if existing is None:
                    return _fail(507, "storage", "the entry vanished after a concurrent store",
                                 retryable=True)
                if existing[1] != request.audio_sha256:
                    return _fail(409, "idempotency_conflict",
                                 "this job_id was already stored with different audio")
                return 200, {"status": "duplicate", "job_id": job_id}
            except OSError as exc:
                self.last_error = f"cannot store {job_id}: {exc}"
                logger.error("voice-note storage failed for %s: %s", job_id, exc)
                return _fail(507, "storage", f"the audio could not be staged: {exc}", retryable=True)
            self.notify()
            logger.info("voice note %s staged (%d bytes, %s)", job_id, len(request.audio), request.codec)
            return 202, {"status": "accepted", "job_id": job_id}
        finally:
            self._release_job_lock(job_id, lock)

    def _reopen(self, request: _Request, job_id: str) -> tuple[int, dict]:
        """A quarantined job re-uploaded with the same audio: requeue it (§5.3)."""
        try:
            self._requeue(job_id)
        except FileExistsError:  # someone moved it on between the lookup and now
            return 200, {"status": "duplicate", "job_id": job_id}
        except (OSError, ValueError) as exc:
            logger.error("voice note %s cannot be requeued: %s", job_id, exc)
            return _fail(507, "storage", f"the quarantined job could not be reopened: {exc}",
                         retryable=True)
        self.notify()
        logger.info("voice note %s was quarantined; reopened and requeued", job_id)
        return 202, {"status": "accepted", "job_id": job_id, "requeued": True}

    # -- idempotency -----------------------------------------------------------------

    @staticmethod
    def job_id_for(request: _Request) -> str:
        """§5.1: a supplied key excludes the content on purpose (a firmware reusing
        one key for different audio is a bug and surfaces as 409 on re-upload)."""
        if request.idempotency_key is not None:
            seed = f"{request.device_id}|{request.idempotency_key}"
        else:
            seed = f"{request.device_id}|{request.recorded_at_utc}|{request.audio_sha256}"
        return hashlib.sha256(seed.encode()).hexdigest()

    def _job_lock(self, job_id: str) -> threading.Lock:
        """One lock per job id serializes its check-and-store (concurrent uploads
        of the same recording from a retry storm cannot both write)."""
        with self._job_locks_guard:
            lock = self._job_locks.get(job_id)
            if lock is None:
                lock = self._job_locks[job_id] = threading.Lock()
            return lock

    def _release_job_lock(self, job_id: str, lock: threading.Lock) -> None:
        with self._job_locks_guard:
            if self._job_locks.get(job_id) is lock and not lock.locked():
                del self._job_locks[job_id]

    def _existing(self, job_id: str) -> Optional[tuple[str, str]]:
        """``(state, audio_sha256)`` for a job already held in inbox/done/failed."""
        for state, directory in (("inbox", self.inbox), ("done", self.done), ("failed", self.failed)):
            entry = directory / job_id
            if not entry.is_dir():
                continue
            try:
                raw = json.loads((entry / "note.json").read_text(encoding="utf-8"))
                if not isinstance(raw, dict):
                    raise ValueError("note.json does not hold an object")
                return state, str(raw.get("audio_sha256") or "")
            except (OSError, ValueError) as exc:
                raise _Inconsistent(f"{state}/{job_id}/note.json is unreadable: {exc}") from exc
        return None

    # -- storage ---------------------------------------------------------------------

    def _note_json(self, job_id: str, request: _Request) -> bytes:
        """The entry's ``note.json`` (§5.2): everything the pipeline reads, plus
        the raw ``recorded_at`` the idempotency key may be derived from."""
        note = {
            "job_id": job_id,
            "device_id": request.device_id,
            "recorded_at": request.recorded_at_raw,
            "recorded_at_utc": request.recorded_at_utc,
            "duration_seconds": request.duration_seconds,
            "audio_sha256": request.audio_sha256,
            "codec": request.codec,
            "supplied_key": request.idempotency_key,
            "attempts": 0,
            "last_error": None,
            "accepted_unix": int(time.time()),
        }
        return json.dumps(note, indent=2, sort_keys=True).encode()

    def _store(self, job_id: str, request: _Request) -> None:
        """Stage then publish the entry (§5.7 steps 2-3).

        ``recording.<codec>`` and ``note.json`` are written and fsynced under
        ``.staging/<job_id>-*`` and the whole directory is renamed into
        ``inbox/<job_id>``: an inbox entry is complete or absent, never partial.
        Any failure removes the staging directory and raises.
        """
        self.inbox.mkdir(parents=True, exist_ok=True)
        self._staging.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(dir=self._staging, prefix=f"{job_id}-"))
        try:
            _write_durable(staging / f"recording.{request.codec}", request.audio)
            _write_durable(staging / "note.json", self._note_json(job_id, request))
            _fsync_dir(staging)
            target = self.inbox / job_id
            try:
                _retry_transient(lambda: os.rename(staging, target))
            except OSError as exc:
                if target.is_dir():  # a concurrent upload won the rename
                    raise FileExistsError(job_id) from exc
                raise
            _fsync_dir(self.inbox)
        except BaseException:
            shutil.rmtree(staging, ignore_errors=True)
            raise

    def _requeue(self, job_id: str) -> None:
        """``failed/<job_id>`` -> ``inbox/<job_id>``: reset ``attempts``, clear
        ``.error`` (§5.3). The pipeline treats that as 'retry now'."""
        source = self.failed / job_id
        target = self.inbox / job_id
        note_path = source / "note.json"
        raw = json.loads(note_path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError("note.json does not hold an object")
        raw["attempts"] = 0
        raw["last_error"] = None
        _write_atomic(note_path, json.dumps(raw, indent=2, sort_keys=True).encode())
        (source / ".error").unlink(missing_ok=True)
        self.inbox.mkdir(parents=True, exist_ok=True)
        try:
            _retry_transient(lambda: os.rename(source, target))
        except OSError as exc:
            if target.is_dir():
                raise FileExistsError(job_id) from exc
            raise
        _fsync_dir(self.inbox)
        _fsync_dir(self.failed)

    # -- maintenance -----------------------------------------------------------------

    def set_paused(self, paused: bool) -> None:
        self.paused = bool(paused)
        logger.info("voice-note endpoint %s", "paused" if paused else "resumed")

    def counts(self) -> dict[str, int]:
        def count(directory: Path) -> int:
            try:
                return sum(1 for p in directory.iterdir() if p.is_dir() and not p.name.startswith("."))
            except OSError:
                return 0
        return {"inbox": count(self.inbox), "done": count(self.done), "failed": count(self.failed)}

    def health(self) -> dict[str, Any]:
        return {"status": "paused" if self.paused else "ok", "port": self.bound_port,
                "last_error": self.last_error, **self.counts()}


# -- the ASGI app ----------------------------------------------------------------------


def create_app(server: NotesServer, page_dir: Optional[Path | str] = None):
    """The FastAPI app: thin enough that the contract lives in ``NotesServer``
    (tests drive it directly); this layer only bounds the body and maps codes.

    ``page_dir`` additionally serves the phone's relay page (``web/ble-note-relay``)
    at ``/`` from this same origin, which is what the container does: the page then
    defaults its notes endpoint to ``/notes/v1`` on the host it came from, and the
    phone needs one address, not two.
    """
    try:  # only consumer of FastAPI; the rest of the contract runs without it (a bare
        # test env has no FastAPI and skips the route tests; Hermes ships it for the server)
        from fastapi import FastAPI, Request
        from fastapi.responses import JSONResponse, Response
        from fastapi.staticfiles import StaticFiles
        from starlette.exceptions import HTTPException as StarletteHTTPException
    except ImportError as exc:  # pragma: no cover - Hermes ships FastAPI
        raise RuntimeError("FastAPI is required to serve /notes/v1 (it ships with Hermes)") from exc
    # The route annotations below are strings (this module has `from __future__ import
    # annotations`) and FastAPI resolves them in the module's globals, not this frame.
    globals().update(FastAPI=FastAPI, Request=Request, JSONResponse=JSONResponse, Response=Response,
                     StaticFiles=StaticFiles)

    app = FastAPI(title="Hermes Gadget voice notes", docs_url=None, redoc_url=None, openapi_url=None)

    def reply(status: int, payload: Optional[dict], headers: Optional[Mapping[str, str]] = None) -> "Response":
        if payload is None:
            return Response(status_code=status, headers=headers)
        return JSONResponse(status_code=status, content=payload, headers=headers)

    @app.exception_handler(StarletteHTTPException)
    async def http_error(request: "Request", exc: "StarletteHTTPException"):
        """404/405 keep the contract's envelope; a 405 keeps Starlette's ``Allow``."""
        if exc.status_code == 404:
            status, payload = _fail(404, "not_found", f"no endpoint at {request.url.path}")
        elif exc.status_code == 405:
            status, payload = _fail(405, "method_not_allowed", "only POST is allowed here")
        else:
            status, payload = _fail(exc.status_code, "internal", str(exc.detail),
                                    retryable=exc.status_code >= 500)
        return reply(status, payload, headers=exc.headers)

    @app.exception_handler(Exception)
    async def unhandled(request: "Request", exc: Exception):
        """The one retryable 5xx (§3.2): the device backs off and resends."""
        logger.exception("voice-note upload failed unexpectedly: %s", exc)
        server.last_error = f"{type(exc).__name__}: {exc}"
        return reply(*_fail(500, "internal", "the server could not handle the upload",
                            retryable=True))

    @app.post(NOTE_PATH)
    async def upload(request: "Request"):
        # The clock for the ts window: the moment these headers arrived, which is
        # also when the device stamped X-Gadget-Ts. The body may take far longer
        # than the window to stream (t_1ea185ed), so this must not be re-read
        # after the spool (see ``_authorize``).
        arrived_at = time.time()
        headers = {str(k).lower(): str(v) for k, v in request.headers.items()}
        raw_length = headers.get("content-length")
        try:
            length = int(raw_length) if raw_length is not None else None
        except ValueError:
            return reply(*_fail(400, "bad_request", "Content-Length is not a decimal number"))
        # Answer 400/413 before buffering anything (§2.4, §4 steps 2-3).
        early = server.precheck(headers, headers.get("content-type"), length)
        if early is not None:
            return reply(*early)
        # The body is spooled to the staging area instead of a bytearray: a note
        # may be as large as the device's own ceiling (2 GiB, MAX_NOTE_BYTES), and
        # holding the body, a copy of the audio part and the hasher at once is
        # what would take the host down. The spool lives on the same filesystem
        # as inbox/, so an upload that does not fit fails here (507, retryable)
        # before a byte reaches an entry, instead of halfway through staging one.
        try:
            staging = server.staging_dir
            staging.mkdir(parents=True, exist_ok=True)
            spool = tempfile.NamedTemporaryFile(dir=staging, prefix="request-",
                                                suffix=".part", delete=False)
        except OSError as exc:
            logger.error("voice-note request spool could not be created: %s", exc)
            return reply(*_fail(507, "storage", f"the upload could not be buffered: {exc}",
                                retryable=True))
        try:
            size = 0
            with spool:
                async for chunk in request.stream():
                    size += len(chunk)
                    if size > MAX_REQUEST_BYTES:  # a chunked sender with no length to pre-check
                        return reply(*_fail(413, "too_large",
                                            f"the request exceeds {MAX_REQUEST_BYTES} bytes"))
                    try:
                        spool.write(chunk)
                    except OSError as exc:  # ENOSPC and friends: loud, and retryable (§5.7)
                        logger.error("voice-note request spool write failed: %s", exc)
                        return reply(*_fail(507, "storage",
                                            f"the upload could not be buffered: {exc}", retryable=True))
                spool.flush()
                os.fsync(spool.fileno())
            if size == 0:  # an empty body is the contract's 400, below
                status, payload = await asyncio.to_thread(
                    server.handle_upload, headers, headers.get("content-type"), length, b"", arrived_at)
            else:
                with open(spool.name, "rb") as handle, \
                        mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                    status, payload = await asyncio.to_thread(
                        server.handle_upload, headers, headers.get("content-type"), length, mapped,
                        arrived_at)
        finally:
            try:
                os.unlink(spool.name)
            except OSError:
                pass
        return reply(status, payload)

    @app.get(HEALTH_PATH)
    async def health():
        return reply(200, server.health())

    if page_dir is not None:
        # Last, so `/notes/v1` and `/health` keep their routes: the mount takes
        # everything left, which is the page and its module.
        app.mount("/", StaticFiles(directory=str(page_dir), html=True), name="relay")

    return app
