// See notes_uploader.hpp.
#include "notes_uploader.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "hg/crypto.hpp"
#include "hg/json.hpp"

namespace hgp::notes {
namespace {

const char* TAG = "hg.notes.up";
// One step's share of the file. The uploader is stepped from the UI task, so this
// is also how long a key can wait: 64 KB of socket write on a sleeping station is
// a second or more, 8 KB is a few milliseconds and the loop comes back for the
// keys (2026-11-24).
constexpr size_t kChunk = 16 * 1024;

// Gathers the response body: the contract's answers are small JSON, and an
// answer that is not read out would leave the socket unusable for the next
// note.
void drain(esp_http_client_handle_t client, std::string& body) {
  char buf[256];
  int n = 0;
  size_t guard = 0;
  while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && guard++ < 64) body.append(buf, static_cast<size_t>(n));
}

}  // namespace

Uploader::~Uploader() { cancel(); }

void Uploader::close() {
  close_transfer();
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

void Uploader::close_transfer() {
  if (client_) {
    esp_http_client_close(static_cast<esp_http_client_handle_t>(client_));
    esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(client_));
    client_ = nullptr;
  }
}

void Uploader::cancel() {
  close();
  if (buf_) {
    heap_caps_free(buf_);  // heap_caps keeps plain malloc pointers valid too
    buf_ = nullptr;
  }
  phase_ = Phase::Idle;
  read_ = 0;
  sent_ = 0;
  tail_sent_ = false;
  http_status_ = 0;
  part_read_ = 0;
  part_sent_ = 0;
  parts_.clear();
  part_ = Part{};
  part_header_.clear();
  part_sha256_.clear();
  part_at_ = 0;
  error_.clear();
}

bool Uploader::begin(const Meta& meta, std::string_view audio_path, std::string_view url, int64_t now_s,
                     std::string_view access_token, Signer sign, std::string& error,
                     uint32_t start_part) {
  cancel();
  if (!sign) {
    error = "no signer";
    return false;
  }
  struct stat st{};
  path_.assign(audio_path);
  if (path_.empty() || ::stat(path_.c_str(), &st) != 0 ||
      static_cast<uint64_t>(st.st_size) < kMinAudioBytes) {
    error = "recording.wav is missing or empty";
    return false;
  }
  file_ = std::fopen(path_.c_str(), "rb");
  if (!file_) {
    error = "cannot read recording.wav";
    return false;
  }
  // The transfer buffer lives on the heap: the app task's stack is a few KiB.
  if (!buf_) buf_ = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf_) buf_ = static_cast<uint8_t*>(std::malloc(kChunk));
  if (!buf_) {
    error = "no memory for the upload";
    return false;
  }
  meta_ = meta;
  // The bytes on the card are what the server sees and what the MAC covers. The
  // digest in note.json is left alone: for a segmented note it is not this
  // upload's to recompute, and a part's own digest travels on the wire.
  meta_.bytes = static_cast<uint32_t>(st.st_size);
  file_bytes_ = static_cast<uint32_t>(st.st_size);
  url_.assign(url);
  now_s_ = now_s;
  token_.assign(access_token);
  sign_ = std::move(sign);
  hasher_ = hg::crypto::Sha256();
  read_ = 0;
  sent_ = 0;
  tail_sent_ = false;
  http_status_ = 0;
  error_.clear();
  // The duration the plan works from is the WAV's own header, not note.json's
  // advisory number: the parts must add up to the bytes that are actually sent.
  const size_t head = std::fread(buf_, 1, std::min<size_t>(kChunk, file_bytes_), file_);
  if (!probe_wav_header(buf_, head, wav_)) {
    error = "recording.wav is not a WAV this firmware can send";
    close();
    return false;
  }
  parts_ = plan_parts(wav_);
  if (parts_.size() > kMaxParts) {
    error = "the note is too long to send in parts";
    close();
    return false;
  }
  part_at_ = resume_index(start_part, parts_.size());
  if (!begin_part()) {
    error = "cannot read recording.wav";
    close();
    return false;
  }
  // A note that needs parts asks the hub first, once a boot: an old hub ignores
  // the part fields and would store part 0 as the whole note.
  phase_ = parts_.size() > 1 && hub_parts_ < 0 ? Phase::Probing : Phase::Hashing;
  return true;
}

bool Uploader::begin_part() {
  if (part_at_ >= parts_.size()) return false;
  // A part is read from the open recording: a caller that has already closed
  // the transfer gets the retry, not a NULL stream in fseek (t_666a4273).
  if (!file_) return false;
  part_ = parts_[part_at_];
  part_read_ = 0;
  part_sent_ = 0;
  part_sha256_.clear();
  part_header_.clear();
  hasher_ = hg::crypto::Sha256();
  const bool segmented = parts_.size() > 1;
  if (segmented) {
    part_header_ = part_wav_header(wav_, part_.audio_bytes);
    part_header_.shrink_to_fit();
    hasher_.update(reinterpret_cast<const uint8_t*>(part_header_.data()), part_header_.size());
    if (std::fseek(file_, static_cast<long>(part_.audio_offset), SEEK_SET) != 0) return false;
  } else if (std::fseek(file_, 0, SEEK_SET) != 0) {
    return false;
  }
  read_ = 0;
  sent_ = 0;
  return true;
}

void Uploader::degrade_to_single_part() {
  parts_.clear();
  part_.index = 0;
  part_.count = 1;
  part_.offset_s = 0;
  part_.duration_s = meta_.duration_s;
  part_.audio_offset = 0;
  part_.audio_bytes = file_bytes_;
  parts_.push_back(part_);
  part_at_ = 0;
  part_header_.clear();
  part_read_ = 0;
  part_sent_ = 0;
}

bool Uploader::probe_hub_parts() {
  if (hub_parts_ >= 0) return hub_parts_ == 1;
  hub_parts_ = 0;
  const size_t scheme = url_.find("://");
  const size_t path = scheme == std::string::npos ? std::string::npos : url_.find('/', scheme + 3);
  const std::string origin = path == std::string::npos ? url_ : url_.substr(0, path);
  const std::string health = origin + "/health";
  esp_http_client_config_t cfg = {};
  cfg.url = health.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 5000;
  cfg.disable_auto_redirect = true;
  auto client = esp_http_client_init(&cfg);
  if (!client) return false;
  if (!token_.empty()) esp_http_client_set_header(client, "X-Gadget-Token", token_.c_str());
  bool ok = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    if (esp_http_client_fetch_headers(client) >= 0) {
      std::string body;
      char chunk[128];
      int n = 0;
      size_t guard = 0;
      while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0 && guard++ < 16) {
        body.append(chunk, static_cast<size_t>(n));
      }
      ok = esp_http_client_get_status_code(client) == 200 &&
           (body.find("\"note_parts\":1") != std::string::npos ||
            body.find("\"note_parts\": 1") != std::string::npos);
    }
    esp_http_client_close(client);
  }
  esp_http_client_cleanup(client);
  hub_parts_ = ok ? 1 : 0;
  ESP_LOGI(TAG, "hub %s parts (%s)", ok ? "takes" : "does not take", health.c_str());
  return ok;
}

bool Uploader::write_all(const void* data, size_t len) {
  auto client = static_cast<esp_http_client_handle_t>(client_);
  const auto* p = static_cast<const char*>(data);
  size_t off = 0;
  while (off < len) {
    const int n = esp_http_client_write(client, p + off, static_cast<int>(len - off));
    if (n <= 0) {
      error_ = "the connection dropped while sending";
      return false;
    }
    off += static_cast<size_t>(n);
    sent_ += static_cast<uint32_t>(n);
  }
  return true;
}

Uploader::Step Uploader::step() {
  if (phase_ == Phase::Idle) return Step::Retry;
  if (!file_) {
    phase_ = Phase::Finished;
    error_ = "the recording disappeared";
    return Step::Retry;
  }

  if (phase_ == Phase::Probing) {
    if (!probe_hub_parts()) {
      degrade_to_single_part();
      if (!begin_part()) {
        phase_ = Phase::Finished;
        error_ = "cannot read recording.wav";
        return Step::Retry;
      }
    }
    phase_ = Phase::Hashing;
    return Step::Running;
  }

  const bool segmented = parts_.size() > 1;

  if (phase_ == Phase::Hashing) {
    // Bounded to this part: a note in parts reads `part_.audio_bytes`, not the
    // rest of the file (t_666a4273).
    const size_t want = segmented ? part_read_len(part_.audio_bytes, part_read_, kChunk) : kChunk;
    const size_t n = std::fread(buf_, 1, want, file_);
    if (n > 0) {
      hasher_.update(buf_, n);
      if (segmented) part_read_ += static_cast<uint32_t>(n); else read_ += static_cast<uint32_t>(n);
      return Step::Running;
    }
    if (std::ferror(file_)) {
      phase_ = Phase::Finished;
      error_ = "cannot read recording.wav";
      return Step::Retry;
    }
    // Hashing a prefix would sign a digest the server cannot match and send a
    // body that stops early. The card is not giving the file back right now;
    // ask for the retry.
    const uint32_t counted = segmented ? part_read_ : read_;
    const uint32_t expected = segmented ? part_.audio_bytes : file_bytes_;
    if (audio_read_short(counted, expected)) {
      phase_ = Phase::Finished;
      error_ = "cannot read recording.wav: stopped at " + std::to_string(counted) + " of " +
               std::to_string(expected) + " bytes";
      return Step::Retry;
    }
    // The body is read from this part's first PCM byte: the hash pass has just
    // read the part to its end, and for a segmented note that is not byte 0 of
    // the recording, so the single-part rewind is not enough (t_666a4273).
    const long body_start = segmented ? static_cast<long>(part_.audio_offset) : 0L;
    if (std::fseek(file_, body_start, SEEK_SET) != 0) {
      phase_ = Phase::Finished;
      error_ = "cannot read recording.wav";
      return Step::Retry;
    }
    const auto digest = hasher_.finish();
    part_sha256_ = hg::crypto::hex(digest.data(), digest.size());
    if (!segmented) sha256_ = part_sha256_;  // the note's own digest, as v1 sent it
    // The wire copy: this part's duration and digest, and the per-part
    // idempotency key (`<note_id>.<index>`), which the hub checks against the
    // part fields. A single-part note keeps the note's own meta and key.
    Meta wire = meta_;
    wire.sha256 = part_sha256_;
    const Part* part = segmented ? &part_ : nullptr;
    if (segmented) {
      wire.duration_s = part_.duration_s;
      wire.idempotency_key = part_idempotency_key(meta_.note_id, part_.index);
    }
    if (!build_request(wire, "recording.wav", request_, part)) {
      phase_ = Phase::Finished;
      error_ = "note.json is not uploadable";
      return Step::Rejected;
    }
    // The stamp is what the hub's ±300 s clock window is checked against, and
    // the MAC covers it, so it cannot be refreshed once the body starts. Take it
    // here, at the send, not at begin(): hashing a note reads the whole
    // recording from the card first (minutes for a few hundred MB), and a stamp
    // from before that pass is already stale when the headers leave
    // (t_1ea185ed). A part takes its own stamp, so a long note's last part is
    // never signed with the first part's time.
    now_s_ = static_cast<int64_t>(std::time(nullptr));
    const std::string canonical = canonical_string(wire, now_s_, part_sha256_, part);
    const std::string mac = sign_(canonical);
    if (mac.empty()) {
      phase_ = Phase::Finished;
      error_ = "cannot sign the request";
      return Step::Rejected;
    }
    // The body is head + audio + tail and the ESP client declares its length as
    // a signed 32-bit int: a note at or past 2 GiB would arrive as a negative
    // Content-Length and the POST would never start. Capture stops a file short
    // of that (kMaxNoteFileBytes), so this is the note an older image recorded at
    // the old ceiling: it cannot ever be sent, and it is terminal rather than a
    // retry that repeats forever. A part is far below this by construction.
    const uint64_t audio_bytes = part_header_.size() + static_cast<uint64_t>(segmented ? part_.audio_bytes : file_bytes_);
    const uint64_t body = request_.content_length(static_cast<size_t>(audio_bytes));
    if (body > static_cast<uint64_t>(INT32_MAX)) {
      phase_ = Phase::Finished;
      error_ = "the note is " + std::to_string(body) + " bytes, past one upload's " +
               std::to_string(INT32_MAX) + ": it can never be sent";
      return Step::Rejected;
    }
    total_ = static_cast<uint32_t>(body);
    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 2048;
    cfg.keep_alive_enable = true;
    cfg.disable_auto_redirect = true;
    auto client = esp_http_client_init(&cfg);
    if (!client) {
      phase_ = Phase::Finished;
      error_ = "cannot start the HTTP client";
      return Step::Retry;
    }
    client_ = client;
    const std::string content_type = request_.content_type();
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    const std::string ts = std::to_string(now_s_);
    esp_http_client_set_header(client, "X-Gadget-Ts", ts.c_str());
    esp_http_client_set_header(client, "X-Gadget-Mac", mac.c_str());
    if (!token_.empty()) esp_http_client_set_header(client, "X-Gadget-Token", token_.c_str());
    // Content-Length is known before the first byte leaves: nothing is
    // buffered, the request streams out of the file.
    if (esp_http_client_open(client, static_cast<int>(total_)) != ESP_OK) {
      error_ = "cannot reach Hermes";
      close();
      phase_ = Phase::Finished;
      return Step::Retry;
    }
    // 20 s is the right clock for "is Hermes there" (connect and head), and the
    // wrong one for the body: esp_transport applies timeout_ms to every write's
    // poll, so one quiet stretch mid-body aborts the POST and discards everything
    // already streamed -- the 10 s backoff then resends from byte 0.
    //
    // On this board a body that follows a recording can be quiet for 66-81 s
    // before its first bytes leave the chip (t_021f43e2: the hub's SYN-ACK goes
    // unanswered and nothing is transmitted on the notes flow, while the ws flow
    // keeps working; when it clears, the body itself streams at 1.3-1.6 MB/s, so
    // it is the flow's first write that stalls, not the transfer). 60 s still cut
    // two of five measured runs off; 120 s is above every measured stall and
    // still ends a body that has really stopped. A hub that is not there is
    // caught by the 20 s connect/head clock above, which this does not touch.
    // Two minutes of blocking write is two minutes of frozen keys when this runs
    // on the UI task. A stalled body is retried, not waited out (2026-11-24).
    // One chunk's write must not own the main loop for longer than this: the
    // uploader runs on the UI task, so a stalled socket is a frozen key. Two
    // seconds is two orders of magnitude more than an 8 KB chunk needs on a
    // healthy link, and a stall is retried rather than waited out.
    esp_http_client_set_timeout_ms(client, 2000);
    if (!write_all(request_.head.data(), request_.head.size())) {
      close();
      phase_ = Phase::Finished;
      return Step::Retry;
    }
    if (segmented && !write_all(part_header_.data(), part_header_.size())) {
      close();
      phase_ = Phase::Finished;
      return Step::Retry;
    }
    read_ = 0;  // count the body again: the hash pass has its own total
    phase_ = Phase::Sending;
    return Step::Running;
  }

  if (phase_ == Phase::Sending) {
    // Same bound as the hash pass: the body is this part and nothing past it.
    const size_t want = segmented ? part_read_len(part_.audio_bytes, part_sent_, kChunk) : kChunk;
    const size_t n = std::fread(buf_, 1, want, file_);
    if (n > 0) {
      if (segmented) part_sent_ += static_cast<uint32_t>(n); else read_ += static_cast<uint32_t>(n);
      if (!write_all(buf_, n)) {
        close();
        phase_ = Phase::Finished;
        return Step::Retry;
      }
      return Step::Running;
    }
    const uint32_t counted = segmented ? part_sent_ : read_;
    const uint32_t expected = segmented ? part_.audio_bytes : file_bytes_;
    if (audio_read_short(counted, expected)) {
      // Same rule as the hash pass: do not close the body early and wait out a
      // request that can never be answered. Retry this part as it is.
      close();
      phase_ = Phase::Finished;
      error_ = "cannot read recording.wav: stopped at " + std::to_string(counted) + " of " +
               std::to_string(expected) + " bytes";
      return Step::Retry;
    }
    if (!tail_sent_) {
      if (!write_all(request_.tail.data(), request_.tail.size())) {
        close();
        phase_ = Phase::Finished;
        return Step::Retry;
      }
      tail_sent_ = true;
    }
    auto client = static_cast<esp_http_client_handle_t>(client_);
    if (esp_http_client_fetch_headers(client) < 0) {
      error_ = "no answer from Hermes";
      close();
      phase_ = Phase::Finished;
      return Step::Retry;
    }
    std::string body;
    drain(client, body);
    http_status_ = esp_http_client_get_status_code(client);
    // The POST is over; the note is not. The recording stays open for the part
    // after this one (`begin_part` seeks it), and each outcome below closes the
    // whole transfer when there is no next part (t_666a4273).
    close_transfer();
    const SyncResult result = classify_response(http_status_);
    if (segmented) {
      ESP_LOGI(TAG, "note %s part %u/%u: HTTP %d -> %s", meta_.note_id.c_str(), part_.index + 1,
               part_.count, http_status_,
               result == SyncResult::Accepted    ? "accepted"
               : result == SyncResult::Duplicate ? "duplicate"
               : result == SyncResult::Rejected  ? "rejected"
                                                 : "retry");
    } else {
      ESP_LOGI(TAG, "note %s: HTTP %d -> %s", meta_.note_id.c_str(), http_status_,
               result == SyncResult::Accepted    ? "accepted"
               : result == SyncResult::Duplicate ? "duplicate"
               : result == SyncResult::Rejected  ? "rejected"
                                                 : "retry");
    }
    switch (result) {
      case SyncResult::Accepted:
      case SyncResult::Duplicate:
        if (segmented && part_at_ + 1 < parts_.size()) {
          // This part is stored (or was already). The next one is a new POST:
          // hash it, sign it with its own stamp, and stay Running.
          ++part_at_;
          tail_sent_ = false;
          if (!begin_part()) {
            close();
            phase_ = Phase::Finished;
            error_ = "cannot read recording.wav";
            return Step::Retry;
          }
          phase_ = Phase::Hashing;
          return Step::Running;
        }
        close();
        phase_ = Phase::Finished;
        return Step::Done;
      case SyncResult::Rejected:
        close();
        phase_ = Phase::Finished;
        error_ = body.empty() ? "rejected" : body;
        return Step::Rejected;
      case SyncResult::Retry:
        close();
        phase_ = Phase::Finished;
        error_ = body.empty() ? "retry" : body;
        return Step::Retry;
    }
  }
  return Step::Retry;
}

}  // namespace hgp::notes
