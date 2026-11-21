// The offline notes' portable half: note ids and timestamps, note.json, the
// multipart request the uploader sends, the response rules, and the card tree
// (driven against a temp directory: same code the device runs on /sdcard).
// The device-only parts (esp_http_client, the SD mount, the microphone) are
// covered on the board; everything that decides *what* gets sent and *where* a
// note ends up is here.
#include "check.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "notes/notes_capture.hpp"
#include "notes/notes_queue.hpp"
#include "notes/notes_store.hpp"

using namespace hgp::notes;

namespace {

// A fresh temp directory per case; removed on the way out.
struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/hg-notes-XXXXXX";
    const char* made = mkdtemp(tmpl);
    path = made ? made : "/tmp/hg-notes-fallback";
  }
  ~TempDir() {
    std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0) {
      // nothing to do: the temp dir is best-effort
    }
  }
  std::string operator/(std::string_view leaf) const { return path + "/" + std::string(leaf); }
};

Meta sample_meta() {
  Meta m;
  m.note_id = "hg-0123456789abcdef-20261120T020512-0001";
  m.device_id = "hg-0123456789abcdef";
  m.device_name = "EPaper";
  m.firmware = "0.6.0";
  m.created_ms = 1795140312000;
  m.recorded_at = iso8601_utc(1795140312);
  m.duration_s = 8;
  m.idempotency_key = m.note_id;
  m.bytes = 394144;
  m.sha256 = "a3f1";
  m.battery_mv = 4012;
  return m;
}

std::string read_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[512];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

}  // namespace

TEST("the note id is device id, UTC stamp and a counter, and it sorts by time") {
  const std::string a = make_note_id("hg-0123456789abcdef", 1795140312, 1);
  CHECK_EQ(a, std::string("hg-0123456789abcdef-20261120T020512-0001"));
  const std::string b = make_note_id("hg-0123456789abcdef", 1795140313, 2);
  CHECK(a < b);  // directory order == recording order
  // The contract's idempotency-key charset, and within its 128-char cap.
  for (char c : a) CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.');
  CHECK(a.size() <= 128);
  CHECK_EQ(iso8601_utc(1795140312), std::string("2026-11-20T02:05:12Z"));
}

TEST("the WAV header is the RIFF PCM16 mono layout the contract sniffs for") {
  uint8_t h[44];
  wav_header(h, 48000);
  CHECK(std::memcmp(h, "RIFF", 4) == 0);
  CHECK(std::memcmp(h + 8, "WAVE", 4) == 0);
  CHECK(std::memcmp(h + 12, "fmt ", 4) == 0);
  CHECK(std::memcmp(h + 36, "data", 4) == 0);
  const auto le32 = [](const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  };
  const auto le16 = [](const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); };
  CHECK_EQ(le32(h + 4), 36u + 48000u);
  CHECK_EQ(le32(h + 16), 16u);
  CHECK_EQ(le16(h + 20), uint16_t(1));       // PCM
  CHECK_EQ(le16(h + 22), uint16_t(1));       // mono
  CHECK_EQ(le32(h + 24), 24000u);            // sample rate
  CHECK_EQ(le32(h + 28), 48000u);            // byte rate
  CHECK_EQ(le16(h + 32), uint16_t(2));       // block align
  CHECK_EQ(le16(h + 34), uint16_t(16));      // bits
  CHECK_EQ(le32(h + 40), 48000u);            // data size
  CHECK_EQ(wav_seconds(48000), 1u);
  CHECK_EQ(wav_seconds(48000 * 5), 5u);
  CHECK_EQ(wav_seconds(0), 0u);
  CHECK_EQ(wav_seconds(48000 + 47999), 1u);  // a short note still says one second
}

TEST("note.json round-trips every field the uploader resends") {
  const Meta m = sample_meta();
  Meta back;
  CHECK(Meta::parse(m.json(), back));
  CHECK_EQ(back.note_id, m.note_id);
  CHECK_EQ(back.device_id, m.device_id);
  CHECK_EQ(back.device_name, m.device_name);
  CHECK_EQ(back.firmware, m.firmware);
  CHECK_EQ(back.created_ms, m.created_ms);
  CHECK_EQ(back.recorded_at, m.recorded_at);
  CHECK_EQ(back.duration_s, m.duration_s);
  CHECK_EQ(back.idempotency_key, m.idempotency_key);
  CHECK_EQ(back.bytes, m.bytes);
  CHECK_EQ(back.sha256, m.sha256);
  CHECK_EQ(back.battery_mv, m.battery_mv);
  CHECK(m.json().find("\"schema_version\":1") != std::string::npos);
  // A torn file (no id, no timestamp) is not a note.
  Meta torn;
  CHECK(!Meta::parse("{\"bytes\":12}", torn));
  CHECK(!Meta::parse("", torn));
}

TEST("the upload classifies answers the contract's way") {
  CHECK(classify_response(202) == SyncResult::Accepted);
  CHECK(classify_response(200) == SyncResult::Duplicate);
  for (int s : {400, 401, 404, 405, 409, 413, 415, 422}) CHECK(classify_response(s) == SyncResult::Rejected);
  for (int s : {500, 503, 507, 408, 429}) CHECK(classify_response(s) == SyncResult::Retry);
  CHECK(classify_response(0) == SyncResult::Retry);  // timeout, reset, refused
}

TEST("the retry backoff doubles from 5 s and stops at 5 minutes") {
  CHECK_EQ(retry_delay_ms(0), 5000u);
  CHECK_EQ(retry_delay_ms(1), 10000u);
  CHECK_EQ(retry_delay_ms(2), 20000u);
  CHECK_EQ(retry_delay_ms(3), 40000u);
  CHECK_EQ(retry_delay_ms(6), 300000u);
  CHECK_EQ(retry_delay_ms(30), 300000u);
}

TEST("the MAC covers the field strings exactly as sent") {
  Meta m = sample_meta();
  const std::string c = canonical_string(m, 1795140312, m.sha256);
  CHECK_EQ(c, std::string("hermes-gadget/v1|note|hg-0123456789abcdef|1795140312|2026-11-20T02:05:12Z|8|a3f1|"
                          "hg-0123456789abcdef-20261120T020512-0001"));
  m.idempotency_key.clear();
  m.note_id.clear();
  CHECK_EQ(canonical_string(m, 1, "ab"),
           std::string("hermes-gadget/v1|note|hg-0123456789abcdef|1|2026-11-20T02:05:12Z|8|ab"));
}

TEST("the multipart body is audio first, then the wire fields, and its length is exact") {
  const Meta m = sample_meta();
  Request r;
  CHECK(build_request(m, "recording.wav", r));
  // The boundary is deterministic: a retry resends the very same bytes.
  Request again;
  CHECK(build_request(m, "recording.wav", again));
  CHECK_EQ(again.boundary, r.boundary);
  CHECK_EQ(again.head, r.head);

  CHECK_EQ(r.content_type(), std::string("multipart/form-data; boundary=") + r.boundary);
  CHECK_EQ(r.content_length(1000), r.head.size() + 1000 + r.tail.size());
  CHECK(r.head.substr(0, r.boundary.size() + 2) == "--" + r.boundary);
  // The head carries the audio part and nothing after its blank line: the
  // audio bytes follow immediately.
  CHECK(r.head.find("name=\"audio\"; filename=\"recording.wav\"") != std::string::npos);
  CHECK(r.head.substr(r.head.size() - 4) == "\r\n\r\n");
  // The audio part's content is closed by CRLF before the next boundary.
  CHECK(r.tail.rfind("\r\n--" + r.boundary, 0) == 0);
  CHECK(r.tail.find("name=\"device_id\"") != std::string::npos);
  CHECK(r.tail.find("name=\"recorded_at\"") != std::string::npos);
  CHECK(r.tail.find("name=\"duration_seconds\"") != std::string::npos);
  CHECK(r.tail.find("name=\"idempotency_key\"") != std::string::npos);
  CHECK(r.tail.find("name=\"checksum\"") != std::string::npos);
  // Field order is the contract's table order.
  CHECK(r.tail.find("device_id") < r.tail.find("recorded_at"));
  CHECK(r.tail.find("recorded_at") < r.tail.find("duration_seconds"));
  CHECK(r.tail.find("duration_seconds") < r.tail.find("idempotency_key"));
  // The values are the note.json strings, verbatim.
  CHECK(r.tail.find(m.device_id) != std::string::npos);
  CHECK(r.tail.find(m.recorded_at) != std::string::npos);
  // And the body closes with the closing boundary.
  CHECK(r.tail.substr(r.tail.size() - (r.boundary.size() + 6)) == "--" + r.boundary + "--\r\n");
  // Without a checksum the part is absent, not empty: the server treats an
  // empty checksum as a mismatch.
  Meta no_sha = m;
  no_sha.sha256.clear();
  Request bare;
  CHECK(build_request(no_sha, "recording.wav", bare));
  CHECK(bare.tail.find("checksum") == std::string::npos);
  // An uploadable note needs an id, a timestamp and a duration in range.
  Meta bad = m;
  bad.duration_s = 0;
  Request never;
  CHECK(!build_request(bad, "recording.wav", never));
}

TEST("the store keeps a note pending only once note.json is committed") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  CHECK(store.count(NoteState::Pending) == 0);

  const Meta m = sample_meta();
  std::string error;
  CHECK(store.create_note(m.note_id, error));
  // Audio written, metadata not: not a note yet.
  const std::string part = store.audio_path(NoteState::Pending, m.note_id) + ".part";
  FILE* f = std::fopen(part.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) std::fclose(f);
  auto ids = store.pending_ids();
  CHECK(ids.empty());

  CHECK(store.write_meta(m.note_id, m, error));
  ids = store.pending_ids();
  CHECK_EQ(ids.size(), size_t(1));
  CHECK_EQ(ids[0], m.note_id);
  // The commit is a rename: no .part file is left behind.
  struct stat st{};
  CHECK(stat((store.meta_path(NoteState::Pending, m.note_id) + ".part").c_str(), &st) != 0);
  Meta back;
  CHECK(store.read_meta(m.note_id, back));
  CHECK_EQ(back.note_id, m.note_id);
  CHECK_EQ(store.pending_bytes(), uint32_t(0));  // only the .part exists
  CHECK(store.count(NoteState::Pending) == 1);
}

TEST("a note moves pending -> sent or failed, and sent/ is pruned oldest first") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  std::vector<std::string> ids;
  for (unsigned i = 1; i <= 4; ++i) {
    Meta m = sample_meta();
    m.note_id = make_note_id(m.device_id, 1795140312 + static_cast<int64_t>(i), i);
    std::string error;
    CHECK(store.create_note(m.note_id, error));
    CHECK(store.write_meta(m.note_id, m, error));
    ids.push_back(m.note_id);
  }
  CHECK(store.count(NoteState::Pending) == 4);
  CHECK(store.move(ids[0], NoteState::Pending, NoteState::Sent));
  CHECK(store.count(NoteState::Pending) == 3);
  CHECK(store.count(NoteState::Sent) == 1);
  CHECK(store.move(ids[1], NoteState::Pending, NoteState::Failed));
  CHECK(store.count(NoteState::Failed) == 1);
  // Moving again is a no-op, not a lost note.
  CHECK(store.move(ids[0], NoteState::Pending, NoteState::Sent));
  CHECK(store.count(NoteState::Sent) == 1);
  // The old id still reads wherever it lives now.
  Meta back;
  CHECK(store.read_meta(ids[1], back));

  // Prune keeps the newest entries (ids sort by time).
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(store.move(ids[static_cast<size_t>(i == 0 ? 2 : i == 1 ? 3 : 0)], NoteState::Pending, NoteState::Sent));
  }
  CHECK_EQ(store.prune_sent(1), uint32_t(2));
  CHECK_EQ(store.count(NoteState::Sent), uint32_t(1));
  // The newest one survived.
  CHECK_EQ(store.list(NoteState::Sent)[0], ids[3]);
}

TEST("a note recorded before the clock was set is uploaded stamped with the time it is sent") {
  // The board has no RTC: this is a note from 1970-01-01T00:24:37Z, taken on a
  // card that had never synced time (hardware run, t_3d3c3f8d).
  Meta m = sample_meta();
  m.note_id = "hg-0123456789abcdef-19700101T002437-0002";
  m.idempotency_key = m.note_id;
  m.recorded_at = "1970-01-01T00:24:37Z";
  m.created_ms = 1477000;

  bool restamped = false;
  const Meta wire = stamped_for_upload(m, 1795140312, &restamped);  // 2026-11-20T02:05:12Z
  CHECK(restamped);
  CHECK_EQ(wire.recorded_at, std::string("2026-11-20T02:05:12Z"));
  // What dedupe rests on never moves: the key, the id, the audio's digest.
  CHECK_EQ(wire.note_id, m.note_id);
  CHECK_EQ(wire.idempotency_key, m.idempotency_key);
  CHECK_EQ(wire.sha256, m.sha256);
  CHECK_EQ(wire.duration_s, m.duration_s);
  CHECK_EQ(wire.bytes, m.bytes);
  // The vault's filename comes from recorded_at, so it stops saying 1970 ...
  CHECK(wire.recorded_at.find("1970") == std::string::npos);
  // ... and the request carries the new stamp: body and MAC alike.
  Request r;
  CHECK(build_request(wire, "recording.wav", r));
  CHECK(r.tail.find("2026-11-20T02:05:12Z") != std::string::npos);
  CHECK(r.tail.find("1970-01-01T00:24:37Z") == std::string::npos);
  // ... and the MAC covers the new stamp, not the old one (the note id in the
  // canonical string keeps its 1970 date on purpose: the id is the key).
  const std::string canonical = canonical_string(wire, 1795140312, wire.sha256);
  CHECK(canonical.find("2026-11-20T02:05:12Z") != std::string::npos);
  CHECK(canonical.find("1970-01-01T00:24:37Z") == std::string::npos);
}

TEST("a plausible stamp is never re-stamped, and an unset clock stamps nothing") {
  const Meta m = sample_meta();  // recorded 2026-11-20T02:05:12Z
  bool restamped = true;
  const Meta same = stamped_for_upload(m, 1795300000, &restamped);
  CHECK(!restamped);
  CHECK_EQ(same.recorded_at, m.recorded_at);  // byte-identical, as the retries need

  // The clock is still 1970: re-stamping would only write a second 1970 stamp.
  Meta old = m;
  old.recorded_at = "1970-01-01T00:24:37Z";
  restamped = true;
  const Meta kept = stamped_for_upload(old, 1477011, &restamped);
  CHECK(!restamped);
  CHECK_EQ(kept.recorded_at, old.recorded_at);

  // A stamp that is not ours at all (a torn or foreign string) is replaced too:
  // it would only be answered 422.
  Meta torn = m;
  torn.recorded_at = "20/11/2026 02:05";
  restamped = false;
  const Meta fixed = stamped_for_upload(torn, 1795140312, &restamped);
  CHECK(restamped);
  CHECK_EQ(fixed.recorded_at, std::string("2026-11-20T02:05:12Z"));
}

TEST("a timestamp counts as a time only when it is really one from 2024 on") {
  CHECK(stamp_plausible("2024-01-01T00:00:00Z"));  // the boundary is trusted
  CHECK(stamp_plausible("2026-11-20T02:05:12Z"));
  CHECK(!stamp_plausible("2023-12-31T23:59:59Z"));
  CHECK(!stamp_plausible("1970-01-01T00:24:37Z"));
  CHECK(!stamp_plausible(""));
  CHECK(!stamp_plausible("2026-11-20T02:05:12+00:00"));  // an offset is not this shape
  CHECK(!stamp_plausible("2026-11-20 02:05:12Z"));
  CHECK(!stamp_plausible("2026-11-20T02:05Z"));
  CHECK(!stamp_plausible("202_6-11-20T02:05:12Z"));
  // A shape with an impossible month is not a time the server would accept.
  CHECK(!stamp_plausible("2026-13-20T02:05:12Z"));
  CHECK(!stamp_plausible("2026-11-20T25:05:12Z"));

  // The id says the same thing without opening note.json.
  CHECK(note_id_undated("hg-0123456789abcdef-19700101T002437-0002"));
  CHECK(!note_id_undated("hg-0123456789abcdef-20261120T020512-0001"));
  CHECK(note_id_undated(make_note_id("hg-0123456789abcdef", 1477017, 3)));
  CHECK(!note_id_undated(make_note_id("hg-0123456789abcdef", 1795140312, 3)));
  CHECK(!note_id_undated("nonsense"));  // not an id: claims nothing
}

TEST("a recording becomes a WAV file plus note.json, and an abort leaves nothing") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  Meta m = sample_meta();
  m.note_id = make_note_id(m.device_id, 1795140312, 7);

  Capture capture;
  std::string error;
  CHECK(capture.start(store, m.note_id, 300, error));
  std::vector<int16_t> block(2400, 1000);  // 100 ms at 24 kHz
  for (int i = 0; i < 10; ++i) capture.feed(block.data(), block.size());
  CHECK_EQ(capture.bytes(), uint32_t(2400 * 2 * 10));
  CHECK_EQ(capture.seconds(), 1u);
  CHECK(capture.finish(m, error));

  const std::string wav = store.audio_path(NoteState::Pending, m.note_id);
  struct stat st{};
  CHECK(stat(wav.c_str(), &st) == 0);
  CHECK_EQ(static_cast<uint32_t>(st.st_size), 44u + 48000u);
  CHECK(stat((wav + ".part").c_str(), &st) != 0);
  CHECK_EQ(m.bytes, 48000u);
  CHECK_EQ(m.duration_s, 1u);

  const std::string bytes = read_file(wav);
  CHECK(bytes.size() == 44 + 48000);
  CHECK(bytes.compare(0, 4, "RIFF") == 0);
  // The header was patched at the end: the data size in it matches the file.
  const uint32_t data_size = uint32_t(uint8_t(bytes[40])) | (uint32_t(uint8_t(bytes[41])) << 8) |
                             (uint32_t(uint8_t(bytes[42])) << 16) | (uint32_t(uint8_t(bytes[43])) << 24);
  CHECK_EQ(data_size, 48000u);

  // An aborted recording leaves no folder behind.
  Meta other = m;
  other.note_id = make_note_id(m.device_id, 1795140400, 8);
  Capture abandoned;
  CHECK(abandoned.start(store, other.note_id, 300, error));
  abandoned.feed(block.data(), block.size());
  abandoned.abort();
  CHECK(stat(store.note_dir(NoteState::Pending, other.note_id).c_str(), &st) != 0);
}

TEST("a recording stops at its cap, never at a partial sample") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  Meta m = sample_meta();
  m.note_id = make_note_id(m.device_id, 1795140312, 9);
  Capture capture;
  std::string error;
  CHECK(capture.start(store, m.note_id, 10, error));  // 10 s cap
  std::vector<int16_t> block(24000, 5);               // one second
  for (int i = 0; i < 12; ++i) capture.feed(block.data(), block.size());
  CHECK(capture.full());
  CHECK_EQ(capture.bytes(), uint32_t(24000 * 2 * 10));
  CHECK(capture.finish(m, error));
  CHECK_EQ(m.duration_s, 10u);
  CHECK(store.discard(NoteState::Pending, m.note_id));
  CHECK_EQ(store.count(NoteState::Pending), 0u);
}

namespace {

bool path_exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

void write_bytes(const std::string& path, const void* data, size_t len) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  if (!f) return;
  std::fwrite(data, 1, len, f);
  std::fclose(f);
}

void write_stub(const std::string& path, const char* text) { write_bytes(path, text, std::strlen(text)); }

// A note as a finished recording leaves it: a WAV header with one sample of
// audio beside the committed note.json, so the uploader would take it.
bool make_note(Store& store, const std::string& id) {
  std::string error;
  if (!store.create_note(id, error)) return false;
  uint8_t wav[45] = {};
  wav_header(wav, 1);
  wav[44] = 0x80;
  write_bytes(store.audio_path(NoteState::Pending, id), wav, sizeof(wav));
  Meta m = sample_meta();
  m.note_id = id;
  m.idempotency_key = id;
  return store.write_meta(id, m, error);
}

}  // namespace

TEST("a note whose recording is missing or empty is quarantined, not retried") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  const std::string id = make_note_id("hg-0123456789abcdef", 1795140312, 11);

  // A recording that never left its audio behind: note.json is committed and
  // there is no recording.wav. The board produced this from an interrupted
  // test recording (t_29cf3afb), and it used to be rescanned every pass.
  std::string error;
  CHECK(store.create_note(id, error));
  Meta m = sample_meta();
  m.note_id = id;
  m.idempotency_key = id;
  CHECK(store.write_meta(id, m, error));
  CHECK_EQ(store.pending_ids().size(), size_t(1));  // it *is* pending: that is the trap
  CHECK_EQ(store.audio_bytes(NoteState::Pending, id), uint64_t(0));
  CHECK_EQ(store.pending_bytes(), uint32_t(0));     // `notes status` printed "0 B queued"
  CHECK(plan_upload(store.audio_bytes(NoteState::Pending, id)) == QueueAction::Quarantine);

  // A bare 44-byte WAV header is no audio either: the uploader refuses it, so
  // the queue must not hand it over and then requeue it (t_69b49f34).
  uint8_t header[44] = {};
  wav_header(header, 0);
  write_bytes(store.audio_path(NoteState::Pending, id), header, sizeof(header));
  CHECK_EQ(store.audio_bytes(NoteState::Pending, id), uint64_t(44));
  CHECK(plan_upload(store.audio_bytes(NoteState::Pending, id)) == QueueAction::Quarantine);

  // The queue's answer is one move to failed/, which is terminal: the next
  // scan of pending/ no longer sees the note at all.
  CHECK(store.move(id, NoteState::Pending, NoteState::Failed));
  CHECK(store.pending_ids().empty());
  CHECK_EQ(store.count(NoteState::Pending), uint32_t(0));
  CHECK_EQ(store.count(NoteState::Failed), uint32_t(1));

  // One sample past the header is a note the uploader takes, and it stays
  // pending until the server answers.
  const std::string good = make_note_id("hg-0123456789abcdef", 1795140313, 12);
  CHECK(make_note(store, good));
  CHECK_EQ(store.audio_bytes(NoteState::Pending, good), uint64_t(45));
  CHECK(plan_upload(store.audio_bytes(NoteState::Pending, good)) == QueueAction::Upload);
  CHECK_EQ(store.pending_ids().size(), size_t(1));
}

TEST("delete removes a note's audio, then its note.json, then the folder") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  const std::string id = make_note_id("hg-0123456789abcdef", 1795140312, 1);
  const std::string other = make_note_id("hg-0123456789abcdef", 1795140313, 2);
  CHECK(make_note(store, id));
  CHECK(make_note(store, other));
  CHECK_EQ(store.count(NoteState::Pending), uint32_t(2));

  NoteState from = NoteState::Sent;
  CHECK(store.erase(id, &from));
  CHECK(from == NoteState::Pending);
  CHECK(!path_exists(store.note_dir(NoteState::Pending, id)));
  CHECK(!path_exists(store.audio_path(NoteState::Pending, id)));
  // The neighbour is untouched, and so is its metadata.
  CHECK(path_exists(store.audio_path(NoteState::Pending, other)));
  const auto left = store.pending_ids();
  CHECK_EQ(left.size(), size_t(1));
  CHECK_EQ(left[0], other);
  // Deleting it again is "unknown", not a second removal or a crash.
  CHECK(!store.erase(id));
}

TEST("a failed note deletes one at a time, and --failed empties the folder") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());
  std::vector<std::string> failed;
  for (unsigned i = 1; i <= 3; ++i) {
    const std::string id = make_note_id("hg-0123456789abcdef", 1795140312 + static_cast<int64_t>(i), i);
    CHECK(make_note(store, id));
    CHECK(store.move(id, NoteState::Pending, NoteState::Failed));
    failed.push_back(id);
  }
  const std::string pending = make_note_id("hg-0123456789abcdef", 1795140400, 9);
  CHECK(make_note(store, pending));
  const std::string sent = make_note_id("hg-0123456789abcdef", 1795140401, 10);
  CHECK(make_note(store, sent));
  CHECK(store.move(sent, NoteState::Pending, NoteState::Sent));

  // One by hand: the store finds it in failed/ without being told where.
  NoteState from = NoteState::Pending;
  CHECK(store.erase(failed[0], &from));
  CHECK(from == NoteState::Failed);
  CHECK_EQ(store.count(NoteState::Failed), uint32_t(2));

  // The rest of the folder in one go.
  CHECK_EQ(store.erase_failed(), uint32_t(2));
  CHECK_EQ(store.count(NoteState::Failed), uint32_t(0));
  CHECK(!path_exists(store.note_dir(NoteState::Failed, failed[1])));
  CHECK(!path_exists(store.note_dir(NoteState::Failed, failed[2])));
  // pending/ and sent/ are not what --failed touches.
  CHECK_EQ(store.count(NoteState::Pending), uint32_t(1));
  CHECK_EQ(store.count(NoteState::Sent), uint32_t(1));
  CHECK_EQ(store.erase_failed(), uint32_t(0));  // already empty
}

TEST("a half-written note folder deletes, and an id outside the tree does not") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());

  // A recording that died before the commit: audio (or its .part) and no
  // note.json. It is not a note anywhere else, but it must still be
  // removable, or the card keeps a folder forever.
  const std::string partial = make_note_id("hg-0123456789abcdef", 1795140312, 3);
  std::string error;
  CHECK(store.create_note(partial, error));
  write_stub(store.audio_path(NoteState::Pending, partial) + ".part", "half");
  CHECK(store.pending_ids().empty());  // not a note: no metadata
  // It is still a folder on the card, so locate() finds it and delete can go.
  NoteState where = NoteState::Sent;
  CHECK(store.locate(partial, where));
  CHECK(where == NoteState::Pending);
  CHECK(store.erase(partial));
  CHECK(!path_exists(store.note_dir(NoteState::Pending, partial)));
  CHECK(!store.locate(partial, where));

  // A note beside a stray file: the known files go in order, then the folder.
  const std::string junk = make_note_id("hg-0123456789abcdef", 1795140313, 4);
  CHECK(make_note(store, junk));
  write_stub(store.note_dir(NoteState::Pending, junk) + "/desktop.ini", "x");
  CHECK(store.erase(junk));
  CHECK(!path_exists(store.note_dir(NoteState::Pending, junk)));

  // An id that is not on the card, and ids that would walk out of the tree.
  CHECK(!store.erase(make_note_id("hg-0123456789abcdef", 1795140500, 5)));
  CHECK(!store.erase("../sent"));
  CHECK(!store.erase(".."));
  CHECK(!store.erase(".hidden"));
  CHECK(!store.erase(""));
  // Nothing outside was touched.
  CHECK(path_exists(store.state_dir(NoteState::Sent)));
  CHECK(path_exists(store.state_dir(NoteState::Failed)));
}

TEST("the delete answers are the exact strings the console prints") {
  CHECK_EQ(deleted_answer("hg-1"), std::string("@ok deleted hg-1"));
  CHECK_EQ(unknown_note_answer("hg-1"), std::string("@error unknown note hg-1"));
  CHECK_EQ(deleted_failed_answer(0), std::string("@ok deleted 0 failed"));
  CHECK_EQ(deleted_failed_answer(12), std::string("@ok deleted 12 failed"));
}
