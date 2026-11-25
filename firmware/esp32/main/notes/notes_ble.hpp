// Bluetooth note sync: the phone reads a note off the card when there is no
// Wi-Fi at all (a walk, a dead network, a hotspot that never came up).
//
// This file is the protocol, not the radio. `BleSync` takes command lines the
// phone wrote to the control characteristic and produces the packets the link
// notifies on the data characteristic; `BleSource` is the card, which the
// device implements over notes::Store and the host tests over memory. The
// wire format is docs/ble-note-sync.md; the GATT service itself is
// port_ble.cpp.
//
// Design rules:
//   - The device never writes to the card during a transfer: a note stays in
//     pending/ until the *phone* says it delivered it, so an interrupted
//     transfer costs nothing but the bytes.
//   - Stop-and-wait per window: the phone acknowledges a byte offset, and the
//     device resumes from exactly that offset, so a dropped link, a missed
//     notification or a phone that only stored half the window all end the
//     same way: the next `resume` continues from where the phone really is.
//   - Every block carries its own CRC-32 and the header carries the WAV's
//     SHA-256, so a corrupted transfer is caught on the phone, not trusted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hgp::notes {

// What the phone needs to know about a note before it asks for the audio.
struct BleNote {
  std::string id;
  uint32_t bytes = 0;
  uint32_t duration_s = 0;
  std::string sha256;      // lowercase hex of recording.wav (may be empty: a note not uploaded yet)
  std::string recorded_at; // ISO-8601, as note.json stores it
};

// The card, as the transfer needs it. open() reads sequentially: read() walks
// from the last seek().
class BleSource {
 public:
  virtual ~BleSource() = default;
  // False when the card is not mounted: the phone is told "no card" and can
  // try again later.
  virtual bool ready() const = 0;
  // Oldest first, with the metadata the header lists.
  virtual std::vector<BleNote> pending() = 0;
  // Starts a transfer at offset 0 and fills `out` with the note's metadata.
  // False when the id is not a pending note on the card.
  virtual bool open(std::string_view id, BleNote& out) = 0;
  virtual size_t read(uint8_t* out, size_t max) = 0;
  virtual bool seek(uint32_t offset) = 0;
  virtual void close() = 0;
  // The phone confirmed delivery to Hermes: pending/ -> sent/.
  virtual bool mark_synced(std::string_view id) = 0;
};

// CRC-32 (IEEE, the zlib polynomial), used per block. Hex-identical to
// crc32() in the relay page, so a phone can check a block it received.
uint32_t crc32(const uint8_t* data, size_t len);

class BleSync {
 public:
  // The data characteristic's packets, one notification each. False means the
  // link had no room (NimBLE out of buffers) or is gone: the packet is kept and
  // offered again on the next tick.
  using Emit = std::function<bool(const uint8_t* data, size_t len)>;

  // A control command has to fit one ATT write, so the link needs a real MTU.
  static constexpr uint16_t kMinMtu = 64;
  // Blocks outstanding before the phone has to acknowledge.
  static constexpr unsigned kWindow = 16;
  // No acknowledgement for this long: the phone is gone or stuck.
  static constexpr uint32_t kAckTimeoutMs = 15000;
  // How long the phone may take to confirm a fully transferred note.
  static constexpr uint32_t kDoneTimeoutMs = 60000;

  // Packet tags on the data characteristic.
  static constexpr uint8_t kTagHeader = 'H';  // a fragment of the note's header line
  static constexpr uint8_t kTagBlock = 'B';   // one audio block (binary)
  static constexpr uint8_t kTagText = 'L';    // a fragment of an answer line (list, ok)
  static constexpr uint8_t kTagError = 'E';   // a fragment of an error line
  static constexpr uint8_t kTagEnd = 'K';     // the last packet of a note's audio

  // `source` must outlive the object.
  void begin(BleSource* source) { source_ = source; }

  // Authentication for the control link. The notes used to rely on an encrypted,
  // authenticated BLE link - which iOS Web Bluetooth cannot set up (no pairing from
  // a web page), so the phone connected, tried to encrypt, was refused and hung up
  // 40 ms later (2026-11-24). The link is now plain inside the radio window and the
  // protocol carries the proof: `challenge`, then `auth <HMAC-SHA256(device key,
  // "hg-ble|" + nonce) base64>`, and nothing else is answered until it holds.
  using Auth = std::function<bool(const std::string& nonce, const std::string& mac_b64)>;
  void set_auth(Auth auth) { auth_ = std::move(auth); }
  void reset_auth() {
    authed_ = false;
    nonce_.clear();
  }
  bool authed() const { return authed_; }
  // The link's negotiated ATT MTU (23 until the phone asks for more).
  void set_mtu(uint16_t mtu);
  uint16_t mtu() const { return mtu_; }

  // One command line from the control characteristic, without its terminator.
  // Answers are queued; tick() sends them.
  void command(std::string_view line, uint32_t now_ms);
  // Drains what is queued, then reads the next window off the card.
  void tick(uint32_t now_ms, const Emit& emit);
  // The link dropped. The note stays pending; the phone resumes when it can.
  void disconnected();

  struct Progress {
    bool active = false;
    std::string id;
    uint32_t sent = 0;
    uint32_t total = 0;
    unsigned pct = 0;
    std::string error;  // why the last transfer stopped (empty after a clean one)
  };
  const Progress& progress() const { return progress_; }
  // A note left pending/ for sent/ because the phone confirmed it: the screen
  // stops showing it, the mode recounts.
  bool took_delivery() {
    const bool yes = delivered_;
    delivered_ = false;
    return yes;
  }

 private:
  void send_text(uint8_t tag, std::string_view text);
  void queue_packet(std::vector<uint8_t> packet);
  void abort_transfer(std::string error, uint32_t now_ms);
  void answer_list();
  void answer_get(std::string_view id, uint32_t from, uint32_t now_ms);
  void answer_ack(uint32_t offset, uint32_t now_ms);
  void answer_done(std::string_view id);
  void window_started(uint32_t now_ms) { last_ack_ms_ = now_ms; }
  uint32_t block_payload() const;

  BleSource* source_ = nullptr;
  uint16_t mtu_ = 23;
  Auth auth_;              // set by the owner: it holds the device key
  std::string nonce_;      // the challenge this link was given, empty once used
  bool authed_ = false;

  // Packets waiting for the link, and how many of them it has taken.
  std::vector<std::vector<uint8_t>> queue_;
  size_t queue_at_ = 0;

  bool active_ = false;
  std::string id_;
  uint32_t total_ = 0;
  uint32_t offset_ = 0;     // the phone's confirmed position
  unsigned in_flight_ = 0;  // blocks notified since the last acknowledgement
  bool end_sent_ = false;
  uint32_t last_ack_ms_ = 0;

  bool delivered_ = false;
  Progress progress_{};
};

}  // namespace hgp::notes
