// The Bluetooth note-sync protocol. See notes_ble.hpp for the rules and
// docs/ble-note-sync.md for the wire format the phone implements.
#include "notes/notes_ble.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#endif

namespace hgp::notes {
namespace {

// Longest block a 517-byte MTU can carry; bigger links still use this, which
// keeps one stack buffer enough.
constexpr size_t kMaxBlock = 512;

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void put_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

std::string trim(std::string_view text) {
  size_t first = 0;
  size_t last = text.size();
  while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
  while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) --last;
  return std::string(text.substr(first, last - first));
}

// A decimal offset, or false. Rejects what strtoul would happily wrap.
bool parse_offset(std::string_view text, uint32_t& out) {
  if (text.empty() || text.size() > 10) return false;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
    if (value > 0xFFFFFFFFull) return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
  // IEEE 802.3, reflected: the same value zlib, Python's binascii.crc32 and the
  // relay page's table-free loop produce.
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

void BleSync::set_mtu(uint16_t mtu) { mtu_ = mtu < 23 ? 23 : mtu; }

uint32_t BleSync::block_payload() const {
  const uint32_t room = static_cast<uint32_t>(mtu_) - 3 - 11;  // ATT header, packet tag, block header
  return std::min<uint32_t>(room, kMaxBlock);
}

void BleSync::queue_packet(std::vector<uint8_t> packet) { queue_.push_back(std::move(packet)); }

void BleSync::send_text(uint8_t tag, std::string_view text) {
  // A text answer is one line split across notifications, every fragment
  // tagged; the phone concatenates until the newline.
  const size_t room = mtu_ > 3 ? mtu_ - 4 : 16;
  std::string line(text);
  line.push_back('\n');
  for (size_t at = 0; at < line.size(); at += room) {
    const size_t n = std::min(room, line.size() - at);
    std::vector<uint8_t> packet;
    packet.reserve(n + 1);
    packet.push_back(tag);
    packet.insert(packet.end(), line.begin() + static_cast<std::ptrdiff_t>(at),
                  line.begin() + static_cast<std::ptrdiff_t>(at + n));
    queue_packet(std::move(packet));
  }
}

void BleSync::abort_transfer(std::string error, uint32_t now_ms) {
  (void)now_ms;
  if (source_ && active_) source_->close();
  active_ = false;
  in_flight_ = 0;
  end_sent_ = false;
  progress_.active = false;
  progress_.error = error;
  send_text(kTagError, error);
}

void BleSync::answer_list() {
  if (!source_ || !source_->ready()) {
    send_text(kTagError, "no card");
    return;
  }
  const auto notes = source_->pending();
  char head[48];
  std::snprintf(head, sizeof(head), "%u", static_cast<unsigned>(notes.size()));
  send_text(kTagText, head);
  for (const auto& note : notes) {
    const std::string digest = note.sha256.empty() ? std::string("-") : note.sha256;
    const std::string line = note.id + " " + std::to_string(note.bytes) + " " +
                             std::to_string(note.duration_s) + " " + digest + " " + note.recorded_at;
    send_text(kTagText, line);
  }
  // Every text answer carries its tag in the packet's first byte; the line
  // itself stays plain, so the phone only ever strips one layer.
  send_text(kTagText, "END");
}

void BleSync::answer_get(std::string_view id, uint32_t from, uint32_t now_ms) {
  if (!source_ || !source_->ready()) {
    send_text(kTagError, "no card");
    return;
  }
  if (active_) {
    source_->close();
    active_ = false;
    in_flight_ = 0;
    end_sent_ = false;
  }
  // A new transfer starts clean: packets still queued belong to the note that
  // was in flight, and their offsets would land in the wrong file.
  queue_.clear();
  queue_at_ = 0;
  BleNote note;
  if (!source_->open(id, note)) {
    send_text(kTagError, "unknown note " + std::string(id));
    return;
  }
  if (note.bytes == 0) {
    source_->close();
    send_text(kTagError, "empty note " + std::string(id));
    return;
  }
  if (from > note.bytes) {
    source_->close();
    send_text(kTagError, "offset past the end of " + std::string(id));
    return;
  }
  if (from != 0 && !source_->seek(from)) {
    source_->close();
    send_text(kTagError, "cannot resume " + std::string(id));
    return;
  }

  // The header repeats on a resume: the phone may be a different app session
  // than the one that received it the first time.
  const std::string digest = note.sha256.empty() ? std::string("-") : note.sha256;
  send_text(kTagHeader, note.id + " " + std::to_string(note.bytes) + " " +
                            std::to_string(note.duration_s) + " " + digest + " " + note.recorded_at);

  active_ = true;
  id_ = note.id;
  total_ = note.bytes;
  offset_ = from;
  in_flight_ = 0;
  end_sent_ = false;
  last_ack_ms_ = now_ms;
  progress_ = Progress{};
  progress_.active = true;
  progress_.id = id_;
  progress_.total = total_;
  progress_.sent = offset_;
  progress_.pct = total_ ? static_cast<unsigned>(static_cast<uint64_t>(offset_) * 100u / total_) : 0;
}

void BleSync::answer_ack(uint32_t offset, uint32_t now_ms) {
  if (!active_ || !source_) {
    // A late acknowledgement after the transfer ended: nothing to do. Saying so
    // would only make a phone that is already resuming log an error.
    return;
  }
  if (offset > total_) {
    abort_transfer("acknowledgement past the end of the note", now_ms);
    return;
  }
  if (offset != offset_) {
    // Behind: the phone lost packets (or restarted mid-window). Rewind to what
    // it really has.
    if (!source_->seek(offset)) {
      abort_transfer("cannot rewind to the acknowledged offset", now_ms);
      return;
    }
  }
  offset_ = offset;
  in_flight_ = 0;
  last_ack_ms_ = now_ms;
}

void BleSync::answer_done(std::string_view id) {
  if (!source_) {
    send_text(kTagError, "no card");
    return;
  }
  if (active_ && id_ == id) {
    source_->close();
    active_ = false;
    in_flight_ = 0;
    end_sent_ = false;
    progress_.active = false;
  }
  if (!source_->mark_synced(id)) {
    send_text(kTagError, "unknown note " + std::string(id));
    return;
  }
  delivered_ = true;
  progress_ = Progress{};
  send_text(kTagText, "ok synced " + std::string(id));
}

void BleSync::command(std::string_view raw, uint32_t now_ms) {
  const std::string line = trim(raw);
  if (line.empty()) return;
  if (mtu_ < kMinMtu) {
    // Long writes make a longer command possible, but a phone that will not
    // negotiate an MTU cannot run a transfer at a usable speed either.
    send_text(kTagError, "Bluetooth MTU too small");
    return;
  }

  const size_t sp = line.find(' ');
  const std::string cmd = line.substr(0, sp);
  const std::string rest = sp == std::string::npos ? std::string() : trim(line.substr(sp + 1));

  // Everything but the challenge and the protocol probe needs the handshake.
  if (cmd == "challenge") {
    char raw[16];
    // The port hands the app a TRNG (esp_fill_random on the device); on the host
    // this path is only reached by tests, which do not need unpredictability.
#if defined(ESP_PLATFORM)
    for (int i = 0; i < 16; i += 4) {
      const uint32_t word = esp_random();
      std::memcpy(raw + i, &word, 4);
    }
#else
    for (int i = 0; i < 16; ++i) raw[i] = static_cast<char>(i * 37 + 11);
#endif
    nonce_.clear();
    char hex[3];
    for (int i = 0; i < 16; ++i) {
      std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned char>(raw[i]));
      nonce_ += hex;
    }
    authed_ = false;
    send_text(kTagText, "nonce " + nonce_);
    return;
  }
  if (cmd == "auth") {
    if (auth_ && nonce_.size() == 32 && auth_(nonce_, rest)) {
      authed_ = true;
      nonce_.clear();  // one challenge, one answer
      send_text(kTagText, "ok auth");
    } else {
      authed_ = false;
      nonce_.clear();
      send_text(kTagError, "auth failed");
    }
    return;
  }
  if (cmd == "hello" || cmd == "info") {
    char answer[64];
    std::snprintf(answer, sizeof(answer), "ok ble-note-sync/1 mtu %u", static_cast<unsigned>(mtu_));
    send_text(kTagText, answer);
    return;
  }
  if (!authed_) {
    send_text(kTagError, "auth required: challenge, then auth <hmac>");
    return;
  }
  if (cmd == "list") {
    answer_list();
    return;
  }
  if (cmd == "get" || cmd == "resume") {
    if (rest.empty()) {
      send_text(kTagError, "usage: get <note_id>|next [from <offset>]");
      return;
    }
    // `get next` is the oldest pending note: what a phone that only wants to
    // drain the device sends in a loop.
    std::string id = rest;
    uint32_t from = 0;
    const size_t cut = rest.find(' ');
    std::string tail;
    if (cut != std::string::npos) {
      id = trim(rest.substr(0, cut));
      tail = trim(rest.substr(cut + 1));
    }
    if (cmd == "resume" && tail.empty()) {
      send_text(kTagError, "usage: resume <note_id> <offset>");
      return;
    }
    if (!tail.empty()) {
      if (tail.rfind("from ", 0) == 0) tail = trim(tail.substr(5));
      if (!parse_offset(tail, from)) {
        send_text(kTagError, "bad offset " + tail);
        return;
      }
    }
    if (id == "next") {
      if (!source_ || !source_->ready()) {
        send_text(kTagError, "no card");
        return;
      }
      const auto notes = source_->pending();
      if (notes.empty()) {
        send_text(kTagText, "ok nothing pending");
        return;
      }
      id = notes.front().id;
    }
    answer_get(id, from, now_ms);
    return;
  }
  if (cmd == "ack") {
    uint32_t offset = 0;
    if (!parse_offset(rest, offset)) {
      send_text(kTagError, "usage: ack <offset>");
      return;
    }
    answer_ack(offset, now_ms);
    return;
  }
  if (cmd == "done") {
    if (rest.empty()) {
      send_text(kTagError, "usage: done <note_id>");
      return;
    }
    answer_done(rest);
    return;
  }
  if (cmd == "stop") {
    if (active_ && source_) source_->close();
    queue_.clear();
    queue_at_ = 0;
    active_ = false;
    in_flight_ = 0;
    end_sent_ = false;
    progress_.active = false;
    send_text(kTagText, "ok stopped");
    return;
  }
  send_text(kTagError, "unknown command '" + cmd + "'");
}

void BleSync::tick(uint32_t now_ms, const Emit& emit) {
  // What is already queued goes out first: answers and blocks keep their order.
  while (queue_at_ < queue_.size()) {
    const auto& packet = queue_[queue_at_];
    if (!emit(packet.data(), packet.size())) return;  // no buffer, or the link is gone
    ++queue_at_;
  }
  if (queue_at_ == queue_.size()) {
    queue_.clear();
    queue_at_ = 0;
  }

  if (!active_ || !source_) return;

  if (end_sent_) {
    // The phone has every byte; it only has to confirm it stored them.
    if (now_ms - last_ack_ms_ > kDoneTimeoutMs) abort_transfer("the phone did not confirm the note", now_ms);
    return;
  }
  if (in_flight_ > 0 && now_ms - last_ack_ms_ > kAckTimeoutMs) {
    abort_transfer("the phone stopped acknowledging", now_ms);
    return;
  }

  while (in_flight_ < kWindow && offset_ < total_) {
    uint8_t block[kMaxBlock];
    // Never ask for more than the note claims: the file on the card can be
    // longer than `note.bytes` (the uploader and the sync header disagree by a
    // few hundred bytes on a real note), and a read that overshoots makes
    // offset_ pass total_, so the phone's next ack is "past the end of the
    // note" and the transfer aborts before it starts - measured 2026-11-26.
    const uint32_t want = std::min<uint32_t>(block_payload(), total_ - offset_);
    const size_t got = source_->read(block, want);
    if (got == 0) {
      abort_transfer("short read from the card", now_ms);
      return;
    }
    std::vector<uint8_t> packet;
    packet.resize(got + 11);
    packet[0] = kTagBlock;
    put_le32(&packet[1], offset_);
    put_le32(&packet[5], crc32(block, got));
    packet[9] = static_cast<uint8_t>(got);
    packet[10] = static_cast<uint8_t>(got >> 8);
    std::memcpy(&packet[11], block, got);
    queue_packet(std::move(packet));
    offset_ += static_cast<uint32_t>(got);
    ++in_flight_;
    progress_.sent = offset_;
    progress_.pct = total_ ? static_cast<unsigned>(static_cast<uint64_t>(offset_) * 100u / total_) : 0;
  }

  if (offset_ >= total_ && in_flight_ == 0 && !end_sent_) {
    queue_packet(std::vector<uint8_t>{kTagEnd});
    end_sent_ = true;
    last_ack_ms_ = now_ms;
  }
}

void BleSync::disconnected() {
  if (active_ && source_) source_->close();
  active_ = false;
  in_flight_ = 0;
  end_sent_ = false;
  queue_.clear();
  queue_at_ = 0;
  progress_.active = false;
  progress_.error = "Bluetooth disconnected";
}

}  // namespace hgp::notes
