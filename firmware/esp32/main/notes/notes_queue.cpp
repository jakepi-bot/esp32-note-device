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

// -- parts (§2.5) -----------------------------------------------------------------

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string& out, uint32_t v) {
  put16(out, static_cast<uint16_t>(v & 0xFFFF));
  put16(out, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

bool wav_format_ok(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits) {
  return tag == 1 && channels == 1 && bits == 16 && rate >= 8000 && rate <= 48000;
}

}  // namespace

bool probe_wav_header(const uint8_t* data, size_t len, WavLayout& out) {
  if (data == nullptr || len < 12) return false;
  if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return false;
  WavLayout info;
  bool have_fmt = false;
  size_t pos = 12;
  while (pos + 8 <= len) {
    const bool is_fmt = std::memcmp(data + pos, "fmt ", 4) == 0;
    const bool is_data = std::memcmp(data + pos, "data", 4) == 0;
    const uint32_t size = le32(data + pos + 4);
    const size_t body = pos + 8;
    if (is_data && have_fmt && static_cast<uint64_t>(body) + size > len) {
      // A header-only read (a part's own header, or the first 64 KiB of a large
      // recording): the data chunk is believed from its own size, which is what
      // the hub does when it probes a staged part.
      info.data_offset = static_cast<uint32_t>(body);
      info.data_bytes = size;
      const uint32_t align = info.block_align();
      if (align == 0) return false;
      const uint32_t frames = size / align;
      info.duration_s = frames / info.sample_rate;
      if (frames % info.sample_rate) info.duration_s += 1;
      if (info.duration_s == 0) info.duration_s = 1;
      out = info;
      return true;
    }
    if (static_cast<uint64_t>(body) + size > len) return false;
    if (is_fmt) {
      if (size < 16) return false;
      const uint16_t tag = le16(data + body);
      const uint16_t channels = le16(data + body + 2);
      const uint32_t rate = le32(data + body + 4);
      const uint16_t bits = le16(data + body + 14);
      if (!wav_format_ok(tag, channels, rate, bits)) return false;
      info.sample_rate = rate;
      info.channels = channels;
      info.bits = bits;
      have_fmt = true;
    } else if (is_data) {
      if (!have_fmt) return false;
      info.data_offset = static_cast<uint32_t>(body);
      info.data_bytes = size;
      const uint32_t align = info.block_align();
      if (align == 0) return false;
      const uint32_t frames = size / align;
      info.duration_s = frames / info.sample_rate;
      if (frames % info.sample_rate) info.duration_s += 1;  // rounded up, like the hub
      if (info.duration_s == 0) info.duration_s = 1;
      out = info;
      return true;
    }
    pos = body + size + (size & 1);  // chunks are word-aligned
  }
  return false;
}

std::vector<Part> plan_parts(const WavLayout& wav) {
  std::vector<Part> parts;
  const uint32_t align = wav.block_align();
  if (wav.sample_rate == 0 || wav.data_bytes == 0 || align == 0) {
    parts.push_back(Part{0, 1, 0, wav.duration_s, wav.data_offset, wav.data_bytes});
    return parts;
  }
  const uint32_t per_part = kPartSeconds * wav.sample_rate * align;
  uint32_t offset = 0;
  while (offset < wav.data_bytes) {
    uint32_t size = wav.data_bytes - offset;
    if (size > per_part) size = per_part;
    size -= size % align;  // a frame is never cut in half
    if (size == 0) {       // a remainder smaller than one frame: keep it with the last part
      if (parts.empty()) {
        parts.push_back(Part{0, 1, 0, 1, wav.data_offset, wav.data_bytes});
      } else {
        parts.back().audio_bytes += wav.data_bytes - offset;
      }
      break;
    }
    Part part;
    part.index = static_cast<uint32_t>(parts.size());
    part.offset_s = part.index * kPartSeconds;
    part.duration_s = size / align / wav.sample_rate;
    if (size % (wav.sample_rate * align)) part.duration_s += 1;
    if (part.duration_s == 0) part.duration_s = 1;
    part.audio_offset = wav.data_offset + offset;
    part.audio_bytes = size;
    parts.push_back(part);
    offset += size;
  }
  const uint32_t count = static_cast<uint32_t>(parts.size());
  for (auto& part : parts) part.count = count;
  return parts;
}

std::string part_wav_header(const WavLayout& wav, uint32_t audio_bytes) {
  const uint32_t align = wav.block_align();
  std::string header;
  header.reserve(44);
  header += "RIFF";
  put32(header, 36 + audio_bytes);
  header += "WAVE";
  header += "fmt ";
  put32(header, 16);
  put16(header, 1);  // PCM
  put16(header, static_cast<uint16_t>(wav.channels));
  put32(header, wav.sample_rate);
  put32(header, wav.sample_rate * align);
  put16(header, static_cast<uint16_t>(align));
  put16(header, static_cast<uint16_t>(wav.bits));
  header += "data";
  put32(header, audio_bytes);
  return header;
}

std::string part_idempotency_key(std::string_view note_id, uint32_t index) {
  std::string key(note_id);
  key += '.';
  key += std::to_string(index);
  return key;
}

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

int four_digits(std::string_view text, size_t at) {
  return (text[at] - '0') * 1000 + (text[at + 1] - '0') * 100 + (text[at + 2] - '0') * 10 + (text[at + 3] - '0');
}

int days_in_month(int year, int month) {
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month != 2) return kDays[month - 1];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return leap ? 29 : 28;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's algorithm),
// so a stamp can be aged without the C library's timezone rules.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

bool parse_iso8601(std::string_view iso, int64_t& out_unix_s) {
  // One shape, fixed width: anything else is not a time this firmware wrote.
  if (iso.size() != 20) return false;
  const bool shape = iso[4] == '-' && iso[7] == '-' && iso[10] == 'T' && iso[13] == ':' && iso[16] == ':' &&
                     iso[19] == 'Z';
  if (!shape) return false;
  if (!all_digits(iso, 0, 4) || !all_digits(iso, 5, 2) || !all_digits(iso, 8, 2) || !all_digits(iso, 11, 2) ||
      !all_digits(iso, 14, 2) || !all_digits(iso, 17, 2)) {
    return false;
  }
  const int year = four_digits(iso, 0);
  const int month = digits(iso, 5), day = digits(iso, 8), hour = digits(iso, 11), minute = digits(iso, 14),
            second = digits(iso, 17);
  if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;
  if (day < 1 || day > days_in_month(year, month)) return false;
  out_unix_s = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
               static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;
  return true;
}

bool stamp_plausible(std::string_view iso) {
  // The wire form is fixed width, so "earlier than 2024" is a second compare —
  // once the shape and the field ranges are known to be real.
  int64_t stamp = 0;
  return parse_iso8601(iso, stamp) && stamp >= kMinTrustedStampUnix;
}

namespace {

// The 15-character stamp inside a note id (`YYYYMMDDTHHMMSS`), when it has one.
bool id_stamp_text(std::string_view note_id, std::string_view& out) {
  const size_t seq = note_id.rfind('-');
  if (seq == std::string_view::npos || seq < 16) return false;
  const size_t stamp_at = seq - 15;
  if (note_id[stamp_at - 1] != '-') return false;
  out = note_id.substr(stamp_at, 15);
  return true;
}

}  // namespace

bool note_id_stamp(std::string_view note_id, int64_t& out_unix_s) {
  std::string_view stamp;
  if (!id_stamp_text(note_id, stamp)) return false;
  if (!all_digits(stamp, 0, 8) || stamp[8] != 'T' || !all_digits(stamp, 9, 6)) return false;
  std::string iso;
  iso.reserve(20);
  iso.append(stamp.substr(0, 4)).append("-").append(stamp.substr(4, 2)).append("-");
  iso.append(stamp.substr(6, 2)).append("T").append(stamp.substr(9, 2)).append(":");
  iso.append(stamp.substr(11, 2)).append(":").append(stamp.substr(13, 2)).append("Z");
  return parse_iso8601(iso, out_unix_s);
}

bool note_id_undated(std::string_view note_id) {
  // <device_id>-<YYYYMMDDTHHMMSS>-<seq:04u>: the stamp is the 15 characters
  // between the last two dashes, and the device id may hold dashes of its own.
  int64_t stamp = 0;
  if (!note_id_stamp(note_id, stamp)) return false;  // not an id: it claims nothing
  return stamp < kMinTrustedStampUnix;
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

std::string canonical_string(const Meta& meta, int64_t ts, std::string_view audio_sha256_hex,
                             const Part* part) {
  // hermes-gadget/v1|note|<device_id>|<ts>|<recorded_at>|<duration>|<sha>[|<key>]
  //   [|parts=<schema>|<note_id>|<index>|<count>|<offset>]
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
  if (part != nullptr) {
    out += "|parts=";
    out += std::to_string(kPartsSchemaVersion);
    out += '|';
    out += meta.note_id;
    out += '|';
    out += std::to_string(part->index);
    out += '|';
    out += std::to_string(part->count);
    out += '|';
    out += std::to_string(part->offset_s);
  }
  return out;
}

bool build_request(const Meta& meta, std::string_view audio_filename, Request& out,
                   const Part* part) {
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
  if (part != nullptr) {
    // §2.5: the note's identity, where this part sits, and the per-part key.
    // The idempotency_key above is already `<note_id>.<index>` (the uploader
    // sets it on the wire copy of the meta), which the hub checks against
    // these fields.
    part_field(tail, r.boundary, "note_id", meta.note_id);
    part_field(tail, r.boundary, "part_index", std::to_string(part->index));
    part_field(tail, r.boundary, "part_count", std::to_string(part->count));
    part_field(tail, r.boundary, "part_offset", std::to_string(part->offset_s));
  }
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

std::string discarded_answer(std::string_view id) {
  std::string out = "@ok discarded ";
  out += id;
  out += ": no audio";
  return out;
}

std::string pruned_answer(uint32_t sent, uint32_t pending, uint32_t failed) {
  return "@ok pruned " + std::to_string(sent) + " sent, " + std::to_string(pending) +
         " empty pending, " + std::to_string(failed) + " empty failed";
}

}  // namespace hgp::notes
