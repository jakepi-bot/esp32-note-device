// The Bluetooth note-sync protocol (esp32/main/notes/notes_ble.hpp) against a
// phone written in the test: the same packet stream the relay page parses, so
// the wire is checked without a radio. Every case here is a rule the docs state
// (docs/ble-note-sync.md): the transfer is resumable, it never writes to the
// card, and a note leaves pending/ only when the phone says Hermes has it.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "notes/notes_ble.hpp"

namespace {

using hgp::notes::BleNote;
using hgp::notes::BleSource;
using hgp::notes::BleSync;

// A card in memory. Same contract as the device's adapter: pending notes only,
// sequential reads from the last seek, and mark_synced() recording what the
// phone claimed.
class FakeCard final : public BleSource {
 public:
  struct Note {
    std::string id;
    std::vector<uint8_t> audio;
    uint32_t duration_s = 3;
    std::string sha256 = "digest";
    std::string recorded_at = "2026-11-20T08:15:04Z";
  };

  bool mounted = true;
  std::vector<Note> notes;
  std::vector<std::string> synced;  // mark_synced calls that succeeded
  bool fail_mark = false;

  bool ready() const override { return mounted; }

  std::vector<BleNote> pending() override {
    std::vector<BleNote> out;
    if (!mounted) return out;
    for (const auto& note : notes) {
      out.push_back({note.id, static_cast<uint32_t>(note.audio.size()), note.duration_s, note.sha256,
                     note.recorded_at});
    }
    return out;
  }

  bool open(std::string_view id, BleNote& out) override {
    for (const auto& note : notes) {
      if (note.id != id) continue;
      open_ = &note;
      at_ = 0;
      out = {note.id, static_cast<uint32_t>(note.audio.size()), note.duration_s, note.sha256, note.recorded_at};
      return true;
    }
    return false;
  }

  size_t read(uint8_t* out, size_t max) override {
    if (!open_) return 0;
    const size_t left = open_->audio.size() - at_;
    const size_t n = std::min(max, left);
    std::memcpy(out, open_->audio.data() + at_, n);
    at_ += n;
    return n;
  }

  bool seek(uint32_t offset) override {
    if (!open_ || offset > open_->audio.size()) return false;
    at_ = offset;
    return true;
  }

  void close() override { open_ = nullptr; }

  bool mark_synced(std::string_view id) override {
    if (fail_mark) return false;
    for (auto& note : notes) {
      if (note.id != id) continue;
      synced.emplace_back(note.id);
      return true;
    }
    return false;
  }

 private:
  const Note* open_ = nullptr;
  size_t at_ = 0;
};

// The phone: reassembles the text lines, checks every block's CRC and writes
// the audio at the offset it carries.
class Phone {
 public:
  explicit Phone(uint16_t mtu = 185) : mtu(mtu) {}

  uint16_t mtu;
  std::vector<std::string> log;  // completed lines, tag first: "L 2", "E no card"
  std::vector<uint8_t> audio;
  unsigned blocks = 0;
  unsigned bad_crc = 0;
  bool ended = false;
  bool accept = true;   // false: NimBLE has no buffer and the packet stays queued
  size_t largest = 0;

  bool emit(const uint8_t* data, size_t len) {
    if (!accept) return false;
    if (len > largest) largest = len;
    const uint8_t tag = data[0];
    if (tag == BleSync::kTagBlock) {
      if (len < 11) {
        ++bad_crc;
        return true;
      }
      const uint32_t offset = le32(data + 1);
      const uint32_t crc = le32(data + 5);
      const size_t n = static_cast<size_t>(data[9]) | (static_cast<size_t>(data[10]) << 8);
      if (len != n + 11) {
        ++bad_crc;
        return true;
      }
      if (hgp::notes::crc32(data + 11, n) != crc) {
        ++bad_crc;
        return true;
      }
      if (audio.size() < offset + n) audio.resize(offset + n);
      if (offset <= audio.size()) std::memcpy(audio.data() + offset, data + 11, n);
      ++blocks;
      return true;
    }
    if (tag == BleSync::kTagEnd) {
      ended = true;
      return true;
    }
    partial_[tag].append(reinterpret_cast<const char*>(data + 1), len - 1);
    size_t nl;
    while ((nl = partial_[tag].find('\n')) != std::string::npos) {
      log.push_back(std::string(1, static_cast<char>(tag)) + " " + partial_[tag].substr(0, nl));
      partial_[tag].erase(0, nl + 1);
    }
    return true;
  }

  // Lines that start with "L ", "E " or "H " in the order they arrived.
  std::vector<std::string> lines(char tag) const {
    std::vector<std::string> out;
    for (const auto& line : log) {
      if (line.size() > 1 && line[0] == tag && line[1] == ' ') out.push_back(line.substr(2));
    }
    return out;
  }
  // The n-th line with that tag, or "" when it never arrived: a failed
  // expectation reports, it does not index past the vector.
  std::string line(char tag, size_t n) const {
    const auto got = lines(tag);
    return n < got.size() ? got[n] : std::string();
  }
  bool saw(char tag, const std::string& need) const {
    for (const auto& line : lines(tag)) {
      if (line.rfind(need, 0) == 0) return true;
    }
    return false;
  }
  // What a phone acknowledges: everything it has stored.
  uint32_t acked() const { return static_cast<uint32_t>(audio.size()); }

 private:
  static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  }
  std::string partial_[256];
};

// The control link authenticates before it answers anything. The key itself is the
// runtime's (it holds the device key); here the verifier accepts and the handshake
// is walked for real, so the tests exercise the same two commands a phone sends.
void authenticate(BleSync& sync, uint32_t now_ms = 0) {
  // A phone negotiates the MTU before it writes anything, and the device refuses a
  // command below kMinMtu before it looks at what the command is.
  if (sync.mtu() < BleSync::kMinMtu) sync.set_mtu(185);
  sync.set_auth([](const std::string& nonce, const std::string& mac) {
    return nonce.size() == 32 && mac == "test-mac";
  });
  sync.command("challenge", now_ms);
  sync.command("auth test-mac", now_ms);
  // Swallow the handshake's two answers, so a test's line indices are its own
  // commands' and not "nonce ..." and "ok auth".
  sync.tick(now_ms, [](const uint8_t*, size_t) { return true; });
}

void drain(BleSync& sync, Phone& phone, uint32_t now_ms) {
  // The device reads the link's MTU every tick (NotesModeImpl::step_ble), and
  // one tick sends what was queued and queues the next window. The app loop
  // ticks every ~10 ms, so the phone sees several passes before it answers:
  // this is those passes. The window cap (BleSync::kWindow) stops it running on.
  sync.set_mtu(phone.mtu);
  for (int pass = 0; pass < 4; ++pass) {
    sync.tick(now_ms, [&phone](const uint8_t* data, size_t len) { return phone.emit(data, len); });
  }
}

// Runs the transfer the way the relay page does: ask, acknowledge what arrived,
// until the end packet or the phone gives up.
bool transfer(BleSync& sync, Phone& phone, const std::string& command, uint32_t& now_ms,
              unsigned rounds = 2000) {
  sync.command(command, now_ms);
  for (unsigned i = 0; i < rounds; ++i) {
    drain(sync, phone, now_ms);
    if (phone.ended) return true;
    if (phone.acked() == 0 && phone.blocks == 0) {
      // Nothing was offered: an error answer, then.
      if (!phone.lines('E').empty()) return false;
    }
    now_ms += 20;
    sync.command("ack " + std::to_string(phone.acked()), now_ms);
  }
  return false;
}

std::vector<uint8_t> pattern(size_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (size_t i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
  return out;
}

FakeCard card_with(std::vector<FakeCard::Note> notes) {
  FakeCard card;
  card.notes = std::move(notes);
  return card;
}

}  // namespace

TEST("crc32 is the value zlib computes") {
  const std::string text = "123456789";
  CHECK(hgp::notes::crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == 0xCBF43926u);
}

TEST("list names every pending note with what the header will repeat") {
  FakeCard card = card_with({{"dev-20261120T081504-0001", pattern(300), 4, "abc123", "2026-11-20T08:15:04Z"},
                             {"dev-20261120T081600-0002", pattern(64), 2, "def456", "2026-11-20T08:16:00Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  sync.command("list", 0);
  drain(sync, phone, 0);
  CHECK(phone.lines('L').size() == 4);
  CHECK(phone.line('L', 0) == "2");
  CHECK(phone.line('L', 1) == "dev-20261120T081504-0001 300 4 abc123 2026-11-20T08:15:04Z");
  CHECK(phone.line('L', 2) == "dev-20261120T081600-0002 64 2 def456 2026-11-20T08:16:00Z");
  CHECK(phone.line('L', 3) == "END");
}

TEST("a long list line is split across packets and still arrives whole") {
  FakeCard card;
  for (int i = 0; i < 6; ++i) {
    card.notes.push_back({"dev-20261120T08150" + std::to_string(i) + "-0001", pattern(100), 1,
                          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                          "2026-11-20T08:15:04Z"});
  }
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  sync.set_mtu(64);
  Phone phone(64);
  sync.command("list", 0);
  drain(sync, phone, 0);
  CHECK(phone.lines('L').size() == 8);
  CHECK(phone.largest <= 61);  // MTU - 3
}

TEST("get transfers the note byte for byte, at the link's MTU") {
  const auto audio = pattern(5000);
  FakeCard card = card_with({{"dev-20261120T081504-0001", audio, 4, "abc123", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  sync.set_mtu(185);
  Phone phone(185);
  uint32_t now = 0;
  CHECK(transfer(sync, phone, "get dev-20261120T081504-0001", now));
  CHECK(phone.audio == audio);
  CHECK(phone.bad_crc == 0);
  CHECK(phone.blocks == 30);  // 5000 / (185-3-11 = 171) rounded up
  CHECK(phone.saw('H', "dev-20261120T081504-0001 5000 4 abc123 2026-11-20T08:15:04Z"));
  CHECK(phone.largest <= 182);
  // Every byte is out, so the screen reads 100%; the note is still pending/
  // until the phone says Hermes took it (`done`).
  CHECK(sync.progress().pct == 100);
  CHECK(sync.progress().error.empty());
}

TEST("the transfer writes nothing: the note is still pending afterwards") {
  FakeCard card = card_with({{"n1", pattern(500), 1, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  CHECK(transfer(sync, phone, "get n1", now));
  CHECK(card.synced.empty());
  CHECK(card.notes.size() == 1);
  CHECK(!sync.took_delivery());
}

TEST("a window the phone missed is resent from what it really has") {
  const auto audio = pattern(4000);
  FakeCard card = card_with({{"n1", audio, 2, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;

  sync.command("get n1", now);
  drain(sync, phone, now);          // window 1: 16 blocks
  const uint32_t have = phone.acked();
  CHECK(have > 0);
  // The phone lost the last three blocks (a full buffer, a dropped packet):
  // it acknowledges only what it has. The device must rewind, not skip.
  phone.audio.resize(have - 300);
  sync.command("ack " + std::to_string(phone.acked()), now);
  now += 20;
  for (unsigned i = 0; i < 2000 && !phone.ended; ++i) {
    drain(sync, phone, now);
    now += 20;
    if (!phone.ended) sync.command("ack " + std::to_string(phone.acked()), now);
  }
  CHECK(phone.ended);
  CHECK(phone.audio == audio);
  CHECK(phone.bad_crc == 0);
}

TEST("a dropped link resumes at the acknowledged offset") {
  const auto audio = pattern(6000);
  FakeCard card = card_with({{"n1", audio, 3, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;

  sync.command("get n1", now);
  drain(sync, phone, now);
  const uint32_t have = phone.acked();
  sync.command("ack " + std::to_string(have), now);
  now += 20;
  drain(sync, phone, now);
  const uint32_t at_disconnect = phone.acked();
  CHECK(at_disconnect > have);

  sync.disconnected();
  CHECK(!sync.progress().active);
  CHECK(!sync.progress().error.empty());

  // The phone comes back and asks again from what it stored.
  Phone second;
  second.audio = phone.audio;
  sync.command("resume n1 " + std::to_string(second.acked()), now);
  now += 20;
  for (unsigned i = 0; i < 2000 && !second.ended; ++i) {
    drain(sync, second, now);
    now += 20;
    if (!second.ended) sync.command("ack " + std::to_string(second.acked()), now);
  }
  CHECK(second.ended);
  CHECK(second.audio == audio);
  // The header repeats on a resume: the phone may be a fresh app session.
  CHECK(second.saw('H', "n1 6000 3 d 2026-11-20T08:15:04Z"));
}

TEST("an empty note is refused, not transferred") {
  FakeCard card = card_with({{"n1", {}, 1, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  CHECK(!transfer(sync, phone, "get n1", now));
  CHECK(phone.saw('E', "empty note n1"));
  CHECK(phone.blocks == 0);
}

TEST("an unknown id, a bad offset and no card each get their own error") {
  FakeCard card = card_with({{"n1", pattern(100), 1, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  {
    Phone phone;
    sync.command("get nope", 0);
    drain(sync, phone, 0);
    CHECK(phone.saw('E', "unknown note nope"));
  }
  {
    Phone phone;
    sync.command("resume n1 5000", 0);
    drain(sync, phone, 0);
    CHECK(phone.saw('E', "offset past the end of n1"));
  }
  {
    Phone phone;
    card.mounted = false;
    sync.command("list", 0);
    drain(sync, phone, 0);
    CHECK(phone.saw('E', "no card"));
    card.mounted = true;
  }
}

TEST("the control link answers nothing until the device key is proved") {
  // The link is plain (iOS Web Bluetooth cannot pair), so the protocol is the gate:
  // a phone that cannot prove it holds the device key gets no note list at all.
  FakeCard card = card_with({{"n1", pattern(3000), 2, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  sync.set_mtu(185);
  Phone phone;
  uint32_t now = 0;

  sync.command("list", now);
  drain(sync, phone, now);
  CHECK(phone.saw('E', "auth required"));

  // A wrong answer does not open it, and the challenge is spent either way.
  sync.set_auth([](const std::string&, const std::string&) { return false; });
  sync.command("challenge", now);
  sync.command("auth not-the-key", now);
  drain(sync, phone, now);
  CHECK(phone.saw('E', "auth failed"));
  sync.command("list", now);
  drain(sync, phone, now);
  CHECK(!phone.saw('L', "1"));

  // The right one does.
  sync.set_auth([](const std::string& nonce, const std::string& mac) {
    return nonce.size() == 32 && mac == "test-mac";
  });
  sync.command("challenge", now);
  sync.command("auth test-mac", now);
  drain(sync, phone, now);
  CHECK(phone.saw('L', "ok auth"));
  sync.command("list", now);
  drain(sync, phone, now);
  CHECK(phone.saw('L', "END"));
}

TEST("done moves the note out of pending and answers") {
  FakeCard card = card_with({{"n1", pattern(3000), 2, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  CHECK(transfer(sync, phone, "get n1", now));
  sync.command("ack " + std::to_string(phone.acked()), now);
  // The phone stored it and Hermes took it: only then does the device let go.
  sync.command("done n1", now);
  drain(sync, phone, now);
  CHECK(card.synced.size() == 1);
  CHECK(card.synced[0] == "n1");
  CHECK(sync.took_delivery());
  CHECK(phone.saw('L', "ok synced n1"));

  // A repeated done (the answer was lost) is still an answer, not an error.
  sync.command("done n1", now);
  drain(sync, phone, now);
  CHECK(card.synced.size() == 2);
  CHECK(!phone.saw('E', ""));
}

TEST("a phone that stops acknowledging ends the transfer, not the note") {
  FakeCard card = card_with({{"n1", pattern(9000), 5, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  sync.command("get n1", now);
  drain(sync, phone, now);
  now += BleSync::kAckTimeoutMs + 1;
  drain(sync, phone, now);
  CHECK(!sync.progress().active);
  CHECK(sync.progress().error == "the phone stopped acknowledging");
  CHECK(phone.saw('E', "the phone stopped acknowledging"));
  CHECK(card.synced.empty());
  CHECK(card.notes.size() == 1);
}

TEST("a link that cannot take a packet stalls without losing it") {
  FakeCard card = card_with({{"n1", pattern(1000), 1, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  phone.accept = false;
  uint32_t now = 0;
  sync.command("get n1", now);
  drain(sync, phone, now);
  CHECK(phone.blocks == 0);
  // The buffers come back: the header is still first, then the blocks.
  phone.accept = true;
  for (unsigned i = 0; i < 200 && !phone.ended; ++i) {
    drain(sync, phone, now);
    now += 20;
    if (!phone.ended) sync.command("ack " + std::to_string(phone.acked()), now);
  }
  CHECK(phone.ended);
  CHECK(phone.audio.size() == 1000);
  CHECK(phone.lines('H').size() == 1);
}

TEST("a small MTU is refused before any bytes move") {
  FakeCard card = card_with({{"n1", pattern(100), 1, "d", "2026-11-20T08:15:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  sync.set_mtu(23);  // the ATT default: a command would not fit a write
  Phone phone(23);
  sync.command("get n1", 0);
  drain(sync, phone, 0);
  CHECK(phone.saw('E', "Bluetooth MTU too small"));
  CHECK(phone.blocks == 0);
}

TEST("get next is the oldest pending note, and an empty queue says so") {
  FakeCard card = card_with({{"older", pattern(200), 1, "d", "2026-11-20T08:15:04Z"},
                             {"newer", pattern(200), 1, "d", "2026-11-20T08:16:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  CHECK(transfer(sync, phone, "get next", now));
  CHECK(phone.saw('H', "older 200 1 d"));
  sync.command("done older", now);
  drain(sync, phone, now);
  CHECK(sync.took_delivery());

  card.notes.erase(card.notes.begin());
  Phone second;
  sync.command("get next", now);
  drain(sync, second, now);
  CHECK(second.saw('H', "newer 200 1 d"));

  card.notes.clear();
  Phone third;
  sync.command("get next", now);
  drain(sync, third, now);
  CHECK(third.saw('L', "ok nothing pending"));
}

TEST("a second get replaces the transfer in flight") {
  const auto first = pattern(3000);
  const auto second_audio = pattern(1200);
  FakeCard card = card_with({{"a", first, 1, "d", "2026-11-20T08:15:04Z"},
                             {"b", second_audio, 1, "e", "2026-11-20T08:16:04Z"}});
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  uint32_t now = 0;
  sync.command("get a", now);
  drain(sync, phone, now);
  Phone other;
  sync.command("get b", now);
  now += 20;
  for (unsigned i = 0; i < 500 && !other.ended; ++i) {
    drain(sync, other, now);
    now += 20;
    if (!other.ended) sync.command("ack " + std::to_string(other.acked()), now);
  }
  CHECK(other.ended);
  CHECK(other.audio == second_audio);
  CHECK(sync.progress().id == "b");
}

TEST("unknown commands get the usage line, not silence") {
  FakeCard card;
  BleSync sync;
  sync.begin(&card);
  authenticate(sync);
  sync.set_mtu(185);  // what a phone negotiates before it writes a command
  Phone phone;
  sync.command("frobnicate", 0);
  sync.command("ack", 0);
  drain(sync, phone, 0);
  CHECK(phone.saw('E', "unknown command 'frobnicate'"));
  CHECK(phone.saw('E', "usage: ack <offset>"));
}
