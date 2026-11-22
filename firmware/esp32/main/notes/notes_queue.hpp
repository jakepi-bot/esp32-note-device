// Offline voice notes: the parts that are decisions, not hardware.
//
// One recording is one note: a `note.json` (the metadata the uploader resends
// verbatim) beside a WAV file, in `pending/` until the server acknowledges it.
// Everything here is plain C++ so the host tests
// (`firmware/tests/test_epaper_notes.cpp`) and the `hg_notes_request` tool
// exercise the same bytes the device sends. Contract:
// docs/voice-note-ingestion.md (wire) and docs/waveshare-epaper-154-v2.md §7
// (card layout).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hgp::notes {

// What the ES8311 records and the contract accepts: 24 kHz PCM16 mono.
constexpr uint32_t kSampleRate = 24000;
constexpr uint32_t kChannels = 1;
constexpr uint32_t kBitsPerSample = 16;
constexpr uint32_t kSchemaVersion = 1;
// The per-note ceiling, and the only practical limit: a recording runs until
// the card is full, and this stops one note at 2 GiB. 2 GiB is 12.4 h of
// 24 kHz PCM16 mono (48 KB/s), and it keeps a single file well inside what
// FAT32 and the RIFF 32-bit size fields handle (4 GiB is their limit).
constexpr size_t kMaxNoteBytes = 2ull * 1024ull * 1024ull * 1024ull;
// duration_seconds is 1..86400 (24 h), the same range the hub's DURATION_MAX
// enforces (plugin/protocol.py NOTE_DURATION_MAX). The byte cap above stops a
// 24 kHz note at 12.4 h, so this is a sanity bound, never the limit a user
// meets: the setting that actually ends a note early is note_max_s.
constexpr uint32_t kMinDurationS = 1;
constexpr uint32_t kMaxDurationS = 24u * 60u * 60u;  // 86400

// One note's `note.json`, in the order the file writes it (spec §7.3). The
// wire fields of the upload contract are kept verbatim here, so a retry is
// byte-identical to the first attempt.
struct Meta {
  std::string note_id;
  std::string device_id;
  std::string device_name;
  std::string firmware;
  int64_t created_ms = 0;      // unix ms, RTC-backed
  std::string recorded_at;     // ISO-8601 with an offset (we emit "…Z"), raw MAC input
  uint32_t duration_s = 0;     // 1..86400 (kMaxDurationS)
  std::string idempotency_key; // = note_id
  uint32_t bytes = 0;          // recording.wav as written
  std::string sha256;          // lowercase hex of recording.wav, filled before the first upload
  uint32_t attempts = 0;       // bumped by the uploader, informational
  int battery_mv = 0;          // 0 = unknown

  std::string json() const;
  static bool parse(std::string_view text, Meta& out);
};

// `<device_id>-<YYYYMMDDTHHMMSS>-<seq:04u>`: time-sortable as a directory
// name, inside the contract's idempotency-key charset (§2).
std::string make_note_id(std::string_view device_id, int64_t unix_s, unsigned seq);

// ISO-8601 in UTC with the Z offset, whole seconds: "2026-11-20T08:15:04Z".
std::string iso8601_utc(int64_t unix_s);

// The board has no RTC: before the first clock sync `std::time()` reads 1970, so
// a note recorded then keeps a 1970 `recorded_at`. That is not a time, only the
// absence of one: a stored stamp earlier than this is replaced at upload.
constexpr int64_t kMinTrustedStampUnix = 1704067200;  // 2024-01-01T00:00:00Z

// True when `iso` is exactly "YYYY-MM-DDTHH:MM:SSZ" (the shape `iso8601_utc`
// writes) naming a second at or after `kMinTrustedStampUnix`. 1970, an offset
// form, a date that does not exist: all false.
bool stamp_plausible(std::string_view iso8601);

// The second `iso` names, when it has the one shape this firmware writes:
// "YYYY-MM-DDTHH:MM:SSZ", in UTC. Unlike stamp_plausible this accepts a stamp
// from before 2024 (the retention rule has to age a note whose clock was never
// set without mistaking 1970 for a very old note). False for any other shape,
// including an offset form and a date that does not exist.
bool parse_iso8601(std::string_view iso8601, int64_t& out_unix_s);

// True when the note id's own stamp (`<device>-YYYYMMDDTHHMMSS-<seq>`) is not
// plausible: the note was recorded before the board had a clock. Reads the
// directory name, so the console can count them without opening note.json.
bool note_id_undated(std::string_view note_id);

// The second the note id's own stamp names (`<device>-YYYYMMDDTHHMMSS-<seq>`),
// without opening the note: the retention rule ages a synced note from this,
// which saves one card read per note. False when the id carries no such stamp.
bool note_id_stamp(std::string_view note_id, int64_t& out_unix_s);

// The meta as it is uploaded. A note with a plausible stamp is returned
// unchanged — its stored bytes and idempotency key must stay put. A note whose
// stamp is implausible gets `now_s`, *if* the clock is now set (a stamp no
// better than the stored one is no improvement); `restamped` reports which
// happened. The note id and the idempotency key never change, so a re-stamped
// note still dedupes to the same job and only the wire and the vault see the
// new time.
Meta stamped_for_upload(const Meta& stored, int64_t now_s, bool* restamped = nullptr);

// What the uploader does with one answer (contract §3, spec §7.3).
enum class SyncResult {
  Accepted,   // 202: the server stored the entry and queued the job
  Duplicate,  // 200: this idempotency key was already stored; also synced
  Rejected,   // a terminal 4xx (auth, contract, checksum): keep the note, stop retrying
  Retry,      // 5xx, no answer, timeout: stay pending under the backoff
};
SyncResult classify_response(int http_status);  // 0 = no response at all
// 5 s, 10 s, 20 s … capped at 300 s (spec §7.3).
uint32_t retry_delay_ms(uint32_t attempts);

// The uploader takes a WAV file of at least the 44-byte header plus a sample;
// anything smaller (or absent) is not audio and never will be. Shared with the
// uploader so the queue and the sender cannot disagree about it.
constexpr uint64_t kMinAudioBytes = 45;

// What the queue does with one pending note before the uploader opens it.
enum class QueueAction : uint8_t {
  Upload,      // the recording is on the card: hand it to the uploader
  Quarantine,  // the recording is missing or empty: terminal, move to failed/
};

// Decided from the audio file's size alone (0 = no file), so the host tests
// and the board run the same rule. A note whose audio is gone cannot be
// uploaded and does not come back: retrying it only re-warns every pass
// (t_69b49f34 — a note left by an interrupted recording warned 30 times a
// second, forever, with `0 B queued` in `notes status`).
QueueAction plan_upload(uint64_t audio_bytes);

// The bytes of one `multipart/form-data` upload (contract §2). `head` ends
// exactly before the audio bytes, so a streamed send is head → audio → tail
// and the Content-Length is known before anything is read from the card.
struct Request {
  std::string boundary;
  std::string head;
  std::string tail;
  std::string canonical;      // contract §2.2, with the timestamp filled by caller

  std::string content_type() const;
  size_t content_length(size_t audio_bytes) const { return head.size() + audio_bytes + tail.size(); }
};

// Builds the multipart framing for `meta`. The MAC input needs the request
// timestamp, which the sender knows at send time, not here: `canonical_for`.
bool build_request(const Meta& meta, std::string_view audio_filename, Request& out);
std::string canonical_string(const Meta& meta, int64_t ts, std::string_view audio_sha256_hex);

// Lowercase hex SHA-256 of `data`.
std::string sha256_hex(const uint8_t* data, size_t len);

// "<name>=<value>" pairs the console's `notes status` prints.
std::string human_bytes(uint64_t bytes);

// The `notes delete` answers, formatted here so the host tests check the exact
// strings the console prints (`notes_mode.cpp` only decides whether to send
// them).
std::string deleted_answer(std::string_view id);       // "@ok deleted <id>"
std::string unknown_note_answer(std::string_view id);  // "@error unknown note <id>"
std::string deleted_failed_answer(uint32_t count);     // "@ok deleted <count> failed"

}  // namespace hgp::notes
