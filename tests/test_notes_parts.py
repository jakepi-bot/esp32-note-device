"""The five-minute part rule (§2.5): one note, several requests.

A recording longer than five minutes is split into parts of at most 300 s and
the hub reassembles them into the note's one ``recording.wav``. The device plans
the same parts in C++ (``firmware/esp32/main/notes/notes_queue.cpp``), so the
numbers are pinned on both sides here: a boundary or a part cap that drifts
between the firmware and the hub would silently truncate or reject a long note.
"""

from __future__ import annotations

import io
import re
import wave

import pytest

from conftest import REPO
from hermes_gadget_plugin import notes_parts

RATE = 24000
NOTE_ID = "hg-0123456789abcdef-20261120T081504-0001"


def wav(seconds: float, *, rate=RATE) -> bytes:
    """A synthetic PCM16 mono WAV of ``seconds`` (24 kHz unless a test says so)."""
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(b"\x00\x01" * int(rate * seconds))
    return buffer.getvalue()


def parts_of(seconds: float, *, rate=RATE):
    data = wav(seconds, rate=rate)
    info = notes_parts.probe_wav(data)
    assert info is not None
    return data, info, notes_parts.plan_parts(info)


def test_twelve_minutes_is_three_parts_of_five_five_and_two():
    data, info, parts = parts_of(12 * 60)
    assert [p.duration_s for p in parts] == [300, 300, 120]
    assert [p.offset_s for p in parts] == [0, 300, 600]
    assert [p.index for p in parts] == [0, 1, 2]
    assert {p.count for p in parts} == {3}
    assert [p.audio_bytes for p in parts] == [300 * RATE * 2] * 2 + [120 * RATE * 2]


def test_four_minutes_fifty_nine_seconds_is_one_part():
    _, _, parts = parts_of(4 * 60 + 59)
    assert len(parts) == 1
    assert parts[0].duration_s == 299
    assert parts[0].count == 1
    assert parts[0].offset_s == 0


def test_five_minutes_exactly_is_still_one_part():
    _, info, parts = parts_of(5 * 60)
    assert len(parts) == 1
    assert parts[0].duration_s == 300
    assert parts[0].audio_bytes == info.data_bytes


def test_one_second_over_five_minutes_is_two_parts():
    _, _, parts = parts_of(5 * 60 + 1)
    assert [p.duration_s for p in parts] == [300, 1]
    assert [p.offset_s for p in parts] == [0, 300]


def test_the_parts_are_the_note_with_no_gap_and_no_overlap():
    data, info, parts = parts_of(12 * 60)
    assert sum(p.audio_bytes for p in parts) == info.data_bytes
    assert parts[0].audio_offset == info.data_offset
    for earlier, later in zip(parts, parts[1:]):
        assert later.audio_offset == earlier.audio_offset + earlier.audio_bytes
        assert later.offset_s == earlier.offset_s + earlier.duration_s


def test_a_part_is_a_complete_wav_the_contract_accepts():
    data, info, parts = parts_of(12 * 60)
    for part in parts:
        blob = notes_parts.slice_part(data, info, part)
        assert blob[:4] == b"RIFF" and blob[8:12] == b"WAVE"
        assert len(blob) == 44 + part.audio_bytes
        probed = notes_parts.probe_wav(blob)
        assert probed is not None
        assert (probed.rate, probed.channels, probed.bits) == (info.rate, info.channels, info.bits)
        assert probed.data_bytes == part.audio_bytes


def test_the_parts_reassemble_into_the_note_byte_for_byte():
    data, info, parts = parts_of(12 * 60)
    blobs = [notes_parts.slice_part(data, info, part) for part in parts]
    assembled = notes_parts.assemble(blobs, info)
    assert assembled is not None
    assert assembled == data


def test_assembled_duration_is_the_sum_not_the_last_part():
    data, info, parts = parts_of(12 * 60)
    blobs = [notes_parts.slice_part(data, info, part) for part in parts]
    assert sum(p.duration_s for p in parts) == 720
    assert notes_parts.probe_wav(notes_parts.assemble(blobs, info)).duration_s == 720


def test_part_fields_carry_the_note_identity_and_the_order():
    _, _, parts = parts_of(12 * 60)
    fields = [notes_parts.part_fields(NOTE_ID, part) for part in parts]
    assert [f["part_index"] for f in fields] == ["0", "1", "2"]
    assert {f["part_count"] for f in fields} == {"3"}
    assert [f["part_offset"] for f in fields] == ["0", "300", "600"]
    assert [f["duration_seconds"] for f in fields] == ["300", "300", "120"]
    assert [f["idempotency_key"] for f in fields] == [f"{NOTE_ID}.{i}" for i in range(3)]
    # Every field is in the contract's charset: a part retries with the same key.
    assert all(f["note_id"] == NOTE_ID for f in fields)


def test_a_truncated_wav_is_refused_unless_only_its_head_is_wanted():
    data = wav(8)
    head = data[:44]
    assert notes_parts.probe_wav(head) is None
    info = notes_parts.probe_wav(head, truncated_ok=True)
    assert info is not None and info.data_bytes == len(data) - 44


def test_a_wav_the_contract_refuses_has_no_plan():
    stereo = io.BytesIO()
    with wave.open(stereo, "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(b"\x00\x01" * 480)
    assert notes_parts.probe_wav(stereo.getvalue()) is None
    assert notes_parts.probe_wav(b"RIFF" + b"\x00" * 40) is None


def test_the_hub_and_the_firmware_plan_with_the_same_numbers():
    source = (REPO / "firmware" / "esp32" / "main" / "notes" / "notes_queue.hpp").read_text(
        encoding="utf-8")
    part_seconds = re.search(r"kPartSeconds\s*=\s*(\d+)", source)
    max_parts = re.search(r"kMaxParts\s*=\s*(\d+)", source)
    assert part_seconds is not None, "the firmware no longer names its part size kPartSeconds"
    assert max_parts is not None, "the firmware no longer names its part cap kMaxParts"
    assert int(part_seconds.group(1)) == notes_parts.PART_SECONDS
    assert int(max_parts.group(1)) == notes_parts.MAX_PARTS


def test_the_part_canonical_segment_is_the_versioned_one_both_sides_sign():
    from hermes_gadget_plugin import protocol

    part = {"version": notes_parts.PARTS_VERSION, "note_id": NOTE_ID, "index": 2,
            "count": 3, "offset": 600}
    canonical = protocol.note_canonical("hg-0123456789abcdef", "1000", "2026-11-20T08:15:04Z",
                                        "120", "a" * 64, f"{NOTE_ID}.2", part)
    assert canonical.endswith(f"|parts=1|{NOTE_ID}|2|3|600")
