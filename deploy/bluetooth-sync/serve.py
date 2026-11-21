#!/usr/bin/env python3
"""The Bluetooth note-sync server as one container process.

One origin serves both halves the phone needs:

* ``GET /`` — the relay page (``web/ble-note-relay``): the Web Bluetooth central
  that takes a note off the e-paper device.
* ``POST /notes/v1`` — the contract endpoint (``docs/voice-note-ingestion.md``)
  the page posts the finished note to.

Because the page is same-origin, its "notes endpoint" field can stay at the
default ``/notes/v1`` and nothing has to be configured on the phone beyond the
device id and key.

Two modes, chosen by the environment:

* **complete** — ``STT_URL`` and ``LLM_URL`` point at OpenAI-compatible HTTP
  services (whisper.cpp's ``/v1/audio/transcriptions``, any ``/v1/chat/completions``).
  The note is transcribed and summarized, then written into the Obsidian vault.
* **stage only** — neither set. Uploads are verified and staged in
  ``notes/inbox/`` exactly as the contract says, and stop there; the audio is
  safe on disk for a host-side pipeline to pick up.

Configuration (all optional except the two directories):

    NOTES_PORT=8766            port inside the container
    NOTES_DIR=/data/notes      inbox/done/failed staging area (mount it)
    VAULT_DIR=/data/vault      Obsidian vault for the finished markdown note
    DEVICES_DIR=/data/gadget   directory holding devices.json (the enrolled keys)
    RELAY_DIR=<installed>/web/ble-note-relay   the page to serve
    GADGET_ACCESS_TOKEN=       only if you set a token on the device too
    STT_URL= STT_MODEL=whisper-1 STT_API_KEY=  STT_LANGUAGE=
    LLM_URL= LLM_MODEL= LLM_API_KEY=
    NOTE_TIMEZONE=America/Chicago   filename local time (default: container TZ)

``python serve.py enroll --key <base64>`` writes a device record (the id is
derived from the key, so a key copied from the host keeps its id) and prints
the id to put in the page.
"""

from __future__ import annotations

import base64
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Optional

from hermes_gadget_plugin import protocol
from hermes_gadget_plugin.notes_ingest import NotesServer
from hermes_gadget_plugin.notes_pipeline import SUMMARY_SYSTEM, PipelineError
from hermes_gadget_plugin.store import DeviceStore

DEFAULT_RELAY = Path(__file__).resolve().parent / "web" / "ble-note-relay"


def _env(name: str, default: str = "") -> str:
    return (os.environ.get(name) or default).strip()


def _post_json(url: str, payload: dict, api_key: str, timeout: float) -> dict:
    body = json.dumps(payload).encode()
    request = urllib.request.Request(url, data=body, method="POST",
                                     headers={"Content-Type": "application/json",
                                              **({"Authorization": f"Bearer {api_key}"} if api_key else {})})
    with urllib.request.urlopen(request, timeout=timeout) as answer:
        raw = answer.read()
    try:
        return json.loads(raw or b"{}")
    except ValueError as exc:
        raise PipelineError(f"{url} answered something that is not JSON", step="transcribe",
                            code="unparseable_output", retryable=True) from exc


def _http_error(exc: urllib.error.HTTPError, step: str) -> PipelineError:
    detail = ""
    try:
        detail = exc.read(400).decode("utf-8", "replace")
    except Exception:  # noqa: BLE001 - the message is a courtesy, not the answer
        detail = str(exc)
    # 4xx is this request's problem (a bad key, a bad model name) unless it is a
    # rate limit; 5xx and timeouts are the service's and deserve a retry.
    retryable = exc.code >= 500 or exc.code in (408, 429)
    return PipelineError(f"{step} service answered {exc.code}: {detail[:300]}", step=step,
                         code=f"{step}_http_{exc.code}", retryable=retryable)


def _transcribe_with(settings: dict):
    """An OpenAI-compatible ``/v1/audio/transcriptions`` call, as the pipeline wants it."""

    def transcribe(audio_path: Path, meta: Any = None) -> str:
        boundary = "----hg" + base64.urlsafe_b64encode(os.urandom(12)).decode().strip("=")
        parts: list[bytes] = []

        def field(name: str, value: str) -> None:
            parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n".encode())
            parts.append(value.encode() + b"\r\n")

        field("model", settings["model"])
        field("response_format", "json")
        if settings["language"]:
            field("language", settings["language"])
        name = Path(audio_path).name
        parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
                     f"Content-Type: {'audio/ogg' if name.endswith('.ogg') else 'audio/wav'}\r\n\r\n".encode())
        parts.append(Path(audio_path).read_bytes() + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        body = b"".join(parts)
        request = urllib.request.Request(
            settings["url"], data=body, method="POST",
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}",
                     **({"Authorization": f"Bearer {settings['api_key']}"} if settings["api_key"] else {})})
        try:
            with urllib.request.urlopen(request, timeout=settings["timeout_s"]) as answer:
                payload = json.loads(answer.read() or b"{}")
        except urllib.error.HTTPError as exc:
            raise _http_error(exc, "transcribe") from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise PipelineError(f"transcribe service unreachable: {exc}", step="transcribe",
                                code="stt_unreachable", retryable=True) from exc
        text = payload.get("text") if isinstance(payload, dict) else None
        if not isinstance(text, str) or not text.strip():
            raise PipelineError("the speech service returned no text", step="transcribe",
                                code="empty_transcript", retryable=True)
        return text.strip()

    return transcribe


def _summarize_with(settings: dict):
    """An OpenAI-compatible ``/v1/chat/completions`` call returning the note's JSON."""

    def summarize(transcript: str) -> dict:
        payload = {"model": settings["model"], "temperature": 0,
                   "messages": [{"role": "system", "content": SUMMARY_SYSTEM},
                                {"role": "user", "content": transcript}]}
        if settings["json_mode"]:
            payload["response_format"] = {"type": "json_object"}
        try:
            answer = _post_json(settings["url"], payload, settings["api_key"], settings["timeout_s"])
        except urllib.error.HTTPError as exc:
            raise _http_error(exc, "summarize") from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise PipelineError(f"summarize service unreachable: {exc}", step="summarize",
                                code="llm_unreachable", retryable=True) from exc
        try:
            content = answer["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise PipelineError("the model answer has no message content", step="summarize",
                                code="unparseable_output", retryable=True) from exc
        text = str(content).strip()
        if text.startswith("```"):
            text = text.split("\n", 1)[-1].rsplit("```", 1)[0].strip()
        try:
            parsed = json.loads(text)
        except ValueError as exc:
            raise PipelineError("the model did not answer with a JSON object", step="summarize",
                                code="unparseable_output", retryable=True) from exc
        if not isinstance(parsed, dict):
            raise PipelineError("the model answered with a JSON value that is not an object",
                                step="summarize", code="unparseable_output", retryable=True)
        return parsed

    return summarize


def _audio_url(base: str) -> str:
    base = base.rstrip("/")
    return base if base.endswith("/audio/transcriptions") else base + "/audio/transcriptions"


def _chat_url(base: str) -> str:
    base = base.rstrip("/")
    return base if base.endswith("/chat/completions") else base + "/chat/completions"


def _enroll(argv: list[str]) -> int:
    """Write one device record so the page has a key to sign with."""
    directory = Path(_env("DEVICES_DIR", "/data/gadget"))
    key_arg = ""
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--key" and args:
            key_arg = args.pop(0)
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            print(f"unknown argument {arg!r}", file=sys.stderr)
            return 2
    key = protocol.decode_key(key_arg) if key_arg else None
    if key is None:
        print("usage: serve.py enroll --key <base64 32-byte device key>\n"
              "The key is the board's, from the host it enrolled with:\n"
              "  python3 -c 'import json,pathlib; print(json.loads(pathlib.Path.home()"
              "\"/.hermes/plugin-data/gadget/devices.json\").read_text()[\"devices\"][\"hg-…\"][\"key\"])'",
              file=sys.stderr)
        return 2
    device_id = protocol.device_id_for_key(key)
    path = directory / DeviceStore.FILENAME
    directory.mkdir(parents=True, exist_ok=True)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("not an object")
    except FileNotFoundError:
        data = {}
    except (OSError, ValueError) as exc:
        print(f"{path} is not readable as a device store: {exc}", file=sys.stderr)
        return 1
    data.setdefault("devices", {})[device_id] = {"key": base64.b64encode(key).decode(),
                                                 "name": "enrolled by the note-sync container",
                                                 "enrolled_at": time.time()}
    path.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    print(f"enrolled {device_id}")
    print("device id for the page: " + device_id)
    print("device key for the page: " + base64.b64encode(key).decode())
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "enroll":
        return _enroll(argv[1:])

    notes_dir = Path(_env("NOTES_DIR", "/data/notes"))
    vault_dir = Path(_env("VAULT_DIR", "/data/vault"))
    devices_dir = Path(_env("DEVICES_DIR", "/data/gadget"))
    relay_dir = Path(_env("RELAY_DIR", str(DEFAULT_RELAY)))
    port = int(_env("NOTES_PORT", "8766"))

    stt_url, llm_url = _env("STT_URL"), _env("LLM_URL")
    transcribe = summarize = None
    if stt_url:
        transcribe = _transcribe_with({"url": _audio_url(stt_url), "model": _env("STT_MODEL", "whisper-1"),
                                       "api_key": _env("STT_API_KEY"),
                                       "language": _env("STT_LANGUAGE") or None,
                                       "timeout_s": float(_env("STT_TIMEOUT_S", "120"))})
    if transcribe and not llm_url:
        # No model: the transcript is the summary, and the note still lands.
        summarize = lambda transcript: {"summary": str(transcript).strip(), "key_points": []}  # noqa: E731
    if llm_url:
        summarize = _summarize_with({"url": _chat_url(llm_url), "model": _env("LLM_MODEL", "gpt-4o-mini"),
                                     "api_key": _env("LLM_API_KEY"),
                                     "json_mode": _env("LLM_JSON_MODE", "1") not in ("0", "false", "no"),
                                     "timeout_s": float(_env("LLM_TIMEOUT_S", "120"))})

    if notes_dir.resolve() == vault_dir.resolve():
        print("NOTES_DIR and VAULT_DIR must differ: one is the staging area, the other the vault",
              file=sys.stderr)
        return 2
    for directory in (notes_dir, vault_dir, devices_dir):
        directory.mkdir(parents=True, exist_ok=True)
    if not (relay_dir / "index.html").is_file():
        print(f"no relay page at {relay_dir}/index.html (set RELAY_DIR)", file=sys.stderr)
        return 2

    server = NotesServer(
        DeviceStore(devices_dir), notes_dir,
        host="0.0.0.0", port=port,
        access_token=_env("GADGET_ACCESS_TOKEN") or None,
        obsidian_dir=vault_dir,
        language=_env("STT_LANGUAGE") or None,
        transcribe=transcribe, summarize=summarize,
        # Transcribe without a model is not a pipeline: stage the note and say so.
        stage_only=transcribe is None,
        page_dir=relay_dir,
    )

    mode = ("complete: whisper at " + stt_url + ", summaries at " + llm_url
            if transcribe and summarize else
            "complete: transcribing at " + stt_url + ", summaries are the transcript's first line"
            if transcribe else
            "stage only: notes are verified and kept in " + str(notes_dir / "inbox")
            + " (set STT_URL and LLM_URL for transcripts)")
    print(f"note sync on :{port}  |  {mode}", flush=True)
    print(f"page at /  |  notes at /notes/v1  |  keys from {devices_dir / 'devices.json'}", flush=True)

    server.start()
    print(f"listening on :{server.bound_port}", flush=True)

    stopping = False

    def stop(signum, _frame):
        nonlocal stopping
        if stopping:
            return
        stopping = True
        print(f"signal {signum}: stopping", flush=True)
        server.stop()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while not stopping:
            time.sleep(0.5)
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
