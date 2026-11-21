// See notes_queue.hpp.
#include "notes_queue.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "hg/crypto.hpp"
#include "hg/json.hpp"

namespace hgp::notes {
namespace {

constexpr char kAudioName[] = "audio";
constexpr char kAudioFilename[] = "recording.wav";

std::string hex12(std::string_view s) {
  const auto digest = hg::crypto::sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return hg::crypto::hex(digest.data(), 12).substr(0, 12);
}

void part_field(std::string& out, std::string_view boundary, const char* name, std::string_view value) {
  out += "--";
  out += boundary;
  out += "\r\nContent-Disposition: form-data; name=\"";
  out += name;
  out += "\"\r\n\r\n";
  out += value;
  out += "\r\n";
}

}  // namespace

std::string Meta::json() const {
  hg::json::Value v = hg::json::Value::object();
  v.set("schema_version", static_cast<unsigned>(kSchemaVersion));
  v.set("note_id", note_id);
  v.set("device_id", device_id);
  v.set("device_name", device_name);
  v.set("firmware", firmware);
  v.set("created_at", static_cast<long long>(created_ms));
  v.set("recorded_at", recorded_at);
  v.set("duration_seconds", static_cast<unsigned>(duration_s));
  v.set("idempotency_key", idempotency_key);
  v.set("sample_rate", static_cast<unsigned>(kSampleRate));
  v.set("channels", static_cast<unsigned>(kChannels));
  v.set("encoding", "wav_pcm16");
  v.set("bytes", static_cast<unsigned>(bytes));
  v.set("sha256", sha256);
  v.set("attempts", static_cast<unsigned>(attempts));
  if (battery_mv > 0) v.set("battery_mv", static_cast<int>(battery_mv));
  v.set("priority", "normal");
  return v.dump();
}

bool Meta::parse(std::string_view text, Meta& out) {
  hg::json::Value v;
  if (!hg::json::parse(text, v) || !v.is_object()) return false;
  Meta m;
  m.note_id = v["note_id"].as_string();
  m.device_id = v["device_id"].as_string();
  m.device_name = v["device_name"].as_string();
  m.firmware = v["firmware"].as_string();
  m.created_ms = v["created_at"].as_int();
  m.recorded_at = v["recorded_at"].as_string();
  m.duration_s = static_cast<uint32_t>(v["duration_seconds"].as_int());
  m.idempotency_key = v["idempotency_key"].as_string();
  m.bytes = static_cast<uint32_t>(v["bytes"].as_int());
  m.sha256 = v["sha256"].as_string();
  m.attempts = static_cast<uint32_t>(v["attempts"].as_int());
  m.battery_mv = static_cast<int>(v["battery_mv"].as_int());
  // A note without an id or a timestamp cannot be uploaded; treat it as torn.
  if (m.note_id.empty() || m.recorded_at.empty()) return false;
  out = std::move(m);
  return true;
}

std::string make_note_id(std::string_view device_id, int64_t unix_s, unsigned seq) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02dT%02d%02d%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec);
  char tail[8];
  std::snprintf(tail, sizeof(tail), "-%04u", seq % 10000u);
  std::string out(device_id);
  out += '-';
  out += stamp;
  out += tail;
  return out;
}

std::string iso8601_utc(int64_t unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  return buf;
}

namespace {

// Two digits at `at`, or -1 when they are not digits.
int digits(std::string_view text, size_t at) {
  if (at + 1 >= text.size()) return -1;
  const char a = text[at], b = text[at + 1];
  if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
  return (a - '0') * 10 + (b - '0');
}

bool all_digits(std::string_view text, size_t at, size_t count) {
  for (size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  return true;
}

}  // namespace

bool stamp_plausible(std::string_view iso) {
  // The wire form is fixed width, so "earlier than 2024" is a string compare —
  // once the shape and the field ranges are known to be real.
  if (iso.size() != 20) return false;
  const bool shape = iso[4] == '-' && iso[7] == '-' && iso[10] == 'T' && iso[13] == ':' && iso[16] == ':' &&
                     iso[19] == 'Z';
  if (!shape) return false;
  if (!all_digits(iso, 0, 4) || !all_digits(iso, 5, 2) || !all_digits(iso, 8, 2) || !all_digits(iso, 11, 2) ||
      !all_digits(iso, 14, 2) || !all_digits(iso, 17, 2)) {
    return false;
  }
  const int month = digits(iso, 5), day = digits(iso, 8), hour = digits(iso, 11), minute = digits(iso, 14),
            second = digits(iso, 17);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
  return iso >= std::string_view("2024-01-01T00:00:00Z");
}

bool note_id_undated(std::string_view note_id) {
  // <device_id>-<YYYYMMDDTHHMMSS>-<seq:04u>: the stamp is the 15 characters
  // between the last two dashes, and the device id may hold dashes of its own.
  const size_t seq = note_id.rfind('-');
  if (seq == std::string_view::npos || seq < 16) return false;
  const size_t stamp_at = seq - 15;
  if (note_id[stamp_at - 1] != '-') return false;
  const std::string_view stamp = note_id.substr(stamp_at, 15);
  if (!all_digits(stamp, 0, 8) || stamp[8] != 'T' || !all_digits(stamp, 9, 6)) return false;
  std::string iso;
  iso.reserve(20);
  iso.append(stamp.substr(0, 4)).append("-").append(stamp.substr(4, 2)).append("-");
  iso.append(stamp.substr(6, 2)).append("T").append(stamp.substr(9, 2)).append(":");
  iso.append(stamp.substr(11, 2)).append(":").append(stamp.substr(13, 2)).append("Z");
  return !stamp_plausible(iso);
}

Meta stamped_for_upload(const Meta& stored, int64_t now_s, bool* restamped) {
  if (restamped) *restamped = false;
  if (stamp_plausible(stored.recorded_at)) return stored;
  // No clock to stamp with either: leave the note exactly as it is. The caller
  // does not upload while the clock is unset, so this only keeps the promise
  // that a re-stamp is an improvement, never a sideways move.
  if (now_s < kMinTrustedStampUnix) return stored;
  Meta out = stored;
  out.recorded_at = iso8601_utc(now_s);
  if (restamped) *restamped = true;
  return out;
}

SyncResult classify_response(int http_status) {
  if (http_status == 200) return SyncResult::Duplicate;
  if (http_status == 202) return SyncResult::Accepted;
  // 400/404/405/413/415/422 mean this recording will never be accepted, and
  // 401/409 mean the key or the device's key does not match: resending the
  // same bytes cannot help, so the note moves to failed/ and stops.
  // 408 and 429 are not the contract's rejections: they say "later", not
  // "never", so they ride the backoff like a 5xx.
  if (http_status == 408 || http_status == 429) return SyncResult::Retry;
  if (http_status >= 400 && http_status < 500) return SyncResult::Rejected;
  // 5xx, 507 storage, 503 paused, 408/429, and 0 (no answer at all: timeout,
  // reset, refused) are all "try again later".
  return SyncResult::Retry;
}

uint32_t retry_delay_ms(uint32_t attempts) {
  constexpr uint32_t kBaseMs = 5000;
  constexpr uint32_t kCapMs = 300000;
  uint32_t delay = kBaseMs;
  for (uint32_t i = 0; i < attempts && delay < kCapMs; ++i) delay *= 2;
  return delay < kCapMs ? delay : kCapMs;
}

QueueAction plan_upload(uint64_t audio_bytes) {
  return audio_bytes >= kMinAudioBytes ? QueueAction::Upload : QueueAction::Quarantine;
}

std::string Request::content_type() const {
  std::string out = "multipart/form-data; boundary=";
  out += boundary;
  return out;
}

std::string canonical_string(const Meta& meta, int64_t ts, std::string_view audio_sha256_hex) {
  // hermes-gadget/v1|note|<device_id>|<ts>|<recorded_at>|<duration>|<sha>[|<key>]
  std::string out = "hermes-gadget/v1|note|";
  out += meta.device_id;
  out += '|';
  out += std::to_string(ts);
  out += '|';
  out += meta.recorded_at;
  out += '|';
  out += std::to_string(meta.duration_s);
  out += '|';
  out += audio_sha256_hex;
  const std::string& key = meta.idempotency_key.empty() ? meta.note_id : meta.idempotency_key;
  if (!key.empty()) {
    out += '|';
    out += key;
  }
  return out;
}

bool build_request(const Meta& meta, std::string_view audio_filename, Request& out) {
  if (meta.device_id.empty() || meta.recorded_at.empty() || meta.duration_s < kMinDurationS) return false;
  Request r;
  // Deterministic per note: a retry may change only the boundary (contract
  // §2.2), and it does not even do that.
  r.boundary = "hg-note-" + hex12(meta.device_id + "|" + meta.note_id);

  // The audio part first, then the wire fields in the contract's table order.
  r.head = "--";
  r.head += r.boundary;
  r.head += "\r\nContent-Disposition: form-data; name=\"";
  r.head += kAudioName;
  r.head += "\"; filename=\"";
  r.head += audio_filename.empty() ? kAudioFilename : std::string(audio_filename);
  r.head += "\"\r\nContent-Type: audio/wav\r\n\r\n";

  // The audio part's bytes end exactly at the tail, and a part's content is
  // followed by CRLF before the next boundary: that CRLF opens the tail.
  std::string tail = "\r\n";
  part_field(tail, r.boundary, "device_id", meta.device_id);
  part_field(tail, r.boundary, "recorded_at", meta.recorded_at);
  part_field(tail, r.boundary, "duration_seconds", std::to_string(meta.duration_s));
  const std::string& key = meta.idempotency_key.empty() ? meta.note_id : meta.idempotency_key;
  if (!key.empty()) part_field(tail, r.boundary, "idempotency_key", key);
  if (!meta.sha256.empty()) part_field(tail, r.boundary, "checksum", meta.sha256);
  tail += "--";
  tail += r.boundary;
  tail += "--\r\n";
  r.tail = std::move(tail);
  out = std::move(r);
  return true;
}

std::string sha256_hex(const uint8_t* data, size_t len) {
  const auto digest = hg::crypto::sha256(data, len);
  return hg::crypto::hex(digest.data(), digest.size());
}

std::string human_bytes(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1024ull * 1024ull)
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  else if (bytes >= 1024ull)
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  return buf;
}

std::string deleted_answer(std::string_view id) {
  std::string out = "@ok deleted ";
  out += id;
  return out;
}

std::string unknown_note_answer(std::string_view id) {
  std::string out = "@error unknown note ";
  out += id;
  return out;
}

std::string deleted_failed_answer(uint32_t count) {
  return "@ok deleted " + std::to_string(count) + " failed";
}

}  // namespace hgp::notes
