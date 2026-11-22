"""Five-minute parts of one voice note (contract §2.5).

A note is one thing to a person — one id in ``notes list``, one vault note, one
``duration_seconds`` — but it does not have to be one HTTP request. A recording
longer than :data:`PART_SECONDS` is uploaded as several requests, each carrying
a complete WAV of at most five minutes and the metadata that says where it
belongs: ``note_id``, ``part_index``, ``part_count`` and ``part_offset``. The
hub stages the parts under ``inbox/<job>/parts/`` and only when the last one
arrives does it concatenate them into the one ``recording.wav`` the pipeline
transcribes.

This module is the one description of that rule. The firmware plans the same
parts in ``firmware/esp32/main/notes/notes_queue.cpp`` (``plan_parts``,
``kPartSeconds``) because the device is the sender; the hub plans nothing it is
not told, but it validates every part against :func:`plan_parts` and it is the
side that assembles. Both are pinned to the same numbers by
``tests/test_notes_parts.py``, so a part count or boundary cannot drift between
the two halves of the wire.

A note at or under five minutes is a single part and is sent exactly as the
contract's v1 upload — no part fields at all — so an old hub still accepts it.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from typing import Optional

#: One part carries at most this many seconds of audio. Five minutes keeps a
#: dropped connection's cost at five minutes of audio instead of the whole note
#: (a 12 min note is three requests, a 2 GiB note is 149).
PART_SECONDS = 300
#: A note of 2 GiB at 24 kHz is 149 parts; 255 is the field's ceiling and the
#: room a note recorded at 8 kHz could need.
MAX_PARTS = 255
#: The version of the *group* of optional parts fields. Absent = a v1 upload.
PARTS_VERSION = 1

_WAV_HEADER = 44


@dataclass(frozen=True)
class WavInfo:
    """Where the PCM lives inside one WAV and what it says about itself."""

    rate: int
    channels: int
    bits: int
    data_offset: int
    data_bytes: int

    @property
    def block_align(self) -> int:
        return max(1, self.channels * (self.bits // 8))

    @property
    def frames(self) -> int:
        return self.data_bytes // self.block_align

    @property
    def duration_s(self) -> int:
        """Whole seconds, rounded up: a part of 0.2 s is still one second to the
        contract, whose ``duration_seconds`` is 1..86400."""
        return max(1, math.ceil(self.frames / self.rate)) if self.rate else 1


@dataclass(frozen=True)
class Part:
    """One request's slice of a note, in the terms both sides use."""

    index: int
    count: int
    offset_s: int  # start of this part, seconds into the note
    duration_s: int  # this part's own audio, seconds
    audio_offset: int  # first PCM byte of the part in the note's recording.wav
    audio_bytes: int  # PCM bytes this part carries


def probe_wav(audio: bytes | memoryview | bytearray, *, truncated_ok: bool = False) -> Optional[WavInfo]:
    """``WavInfo`` for a PCM16 mono WAV the contract accepts, else None.

    Same chunk walk as ``notes_ingest.sniff_codec``: the ``fmt `` chunk decides
    the sample format, the ``data`` chunk decides where the audio is. Chunks are
    word-aligned. A truncated file (a data chunk that promises more than the
    request holds) is refused rather than half-read — unless ``truncated_ok``,
    which is for reading just the head of a staged part to learn its geometry.
    """
    view = memoryview(audio)
    if len(view) < 12 or bytes(view[:4]) != b"RIFF" or bytes(view[8:12]) != b"WAVE":
        return None
    pos = 12
    fmt: Optional[tuple[int, int, int, int]] = None
    while pos + 8 <= len(view):
        chunk = bytes(view[pos:pos + 4])
        size = int.from_bytes(view[pos + 4:pos + 8], "little")
        body_start = pos + 8
        body_end = body_start + size
        if chunk == b"data" and truncated_ok and fmt is not None and body_end > len(view):
            tag, channels, rate, bits = fmt
            if tag != 1 or channels != 1 or bits != 16 or not 8000 <= rate <= 48000:
                return None
            return WavInfo(rate=rate, channels=channels, bits=bits,
                           data_offset=body_start, data_bytes=size)
        if body_end > len(view):
            return None
        if chunk == b"fmt ":
            if size < 16:
                return None
            tag = int.from_bytes(view[body_start:body_start + 2], "little")
            channels = int.from_bytes(view[body_start + 2:body_start + 4], "little")
            rate = int.from_bytes(view[body_start + 4:body_start + 8], "little")
            bits = int.from_bytes(view[body_start + 14:body_start + 16], "little")
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            if fmt is None:
                return None
            tag, channels, rate, bits = fmt
            if tag != 1 or channels != 1 or bits != 16 or not 8000 <= rate <= 48000:
                return None
            return WavInfo(rate=rate, channels=channels, bits=bits,
                           data_offset=body_start, data_bytes=size)
        pos = body_end + (size & 1)  # chunks are word-aligned
    return None


def plan_parts(info: WavInfo) -> list[Part]:
    """The parts one note is sent as: five minutes each, then the remainder.

    Boundaries land on whole frames, so no sample is split between two parts and
    the parts' PCM is the note's PCM end to end. A note at or under
    :data:`PART_SECONDS` is one part — the single-part upload of contract v1.
    """
    if not info.rate or info.data_bytes <= 0:
        return [Part(index=0, count=1, offset_s=0, duration_s=info.duration_s,
                     audio_offset=info.data_offset, audio_bytes=max(0, info.data_bytes))]
    per_part = PART_SECONDS * info.rate * info.block_align  # bytes of one full part
    parts: list[Part] = []
    offset = 0
    while offset < info.data_bytes:
        size = min(per_part, info.data_bytes - offset)
        size -= size % info.block_align  # a frame is never cut in half
        if size <= 0:  # a remainder smaller than a frame: keep it with the last part
            parts[-1] = Part(index=parts[-1].index, count=0,
                             offset_s=parts[-1].offset_s, duration_s=parts[-1].duration_s,
                             audio_offset=parts[-1].audio_offset,
                             audio_bytes=parts[-1].audio_bytes + (info.data_bytes - offset))
            break
        index = len(parts)
        parts.append(Part(index=index, count=0, offset_s=index * PART_SECONDS,
                          duration_s=max(1, math.ceil(size / info.block_align / info.rate)),
                          audio_offset=info.data_offset + offset, audio_bytes=size))
        offset += size
    count = len(parts)
    return [Part(index=p.index, count=count, offset_s=p.offset_s, duration_s=p.duration_s,
                 audio_offset=p.audio_offset, audio_bytes=p.audio_bytes) for p in parts]


def wav_header(info: WavInfo, audio_bytes: int) -> bytes:
    """A 44-byte PCM16 mono RIFF header for ``audio_bytes`` of PCM."""
    block_align = info.block_align
    byte_rate = info.rate * block_align
    body = 36 + audio_bytes
    return (b"RIFF" + struct.pack("<I", body) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, info.channels, info.rate,
                                    byte_rate, block_align, info.bits)
            + b"data" + struct.pack("<I", audio_bytes))


def slice_part(audio: bytes | memoryview, info: WavInfo, part: Part) -> bytes:
    """One part as a complete WAV: its own header, then its PCM range."""
    start = part.audio_offset
    pcm = bytes(memoryview(audio)[start:start + part.audio_bytes])
    return wav_header(info, len(pcm)) + pcm


def assemble(parts: list[bytes], info: WavInfo) -> Optional[bytes]:
    """The note's one ``recording.wav`` from its parts, in index order.

    Every part must be a WAV of the note's own format; None if one is not, which
    is the caller's 4xx rather than a note with a hole in it.
    """
    pcm = bytearray()
    for index, blob in enumerate(parts):
        part_info = probe_wav(blob)
        if part_info is None or (part_info.rate, part_info.channels, part_info.bits) != \
                (info.rate, info.channels, info.bits):
            return None
        pcm += blob[part_info.data_offset:part_info.data_offset + part_info.data_bytes]
    return wav_header(info, len(pcm)) + bytes(pcm)


def part_fields(note_id: str, part: Part) -> dict[str, str]:
    """The wire fields one part carries, as the multipart text parts.

    ``duration_seconds`` is this part's own audio (the contract's field, checked
    against nothing but its range), and the per-part idempotency key makes a
    retried part dedupe to itself. The key's separators stay inside the
    contract's charset: ``<note_id>.<index>``.
    """
    return {
        "note_id": note_id,
        "part_index": str(part.index),
        "part_count": str(part.count),
        "part_offset": str(part.offset_s),
        "duration_seconds": str(part.duration_s),
        "idempotency_key": f"{note_id}.{part.index}",
    }
