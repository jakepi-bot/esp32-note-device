// The upload half of offline notes: one queued note, one POST to
// `http://<hermes-host>:8766/notes/v1` (contract: docs/voice-note-ingestion.md).
//
// The transfer is sliced: `step()` does at most one 64 KiB read/write and
// returns, so the app loop keeps running its buttons, Wi-Fi and live mode while
// a 14 MB note goes out. Phases: the hub's part capability (a note longer than
// five minutes), then SHA-256 of this part (the MAC and `checksum` need it),
// then the multipart request, then the answer's classification.
//
// A note at or under five minutes is one POST and its bytes are exactly the v1
// upload. A longer one is sent as parts of at most `kPartSeconds` (§2.5): each
// part is its own complete WAV plus `note_id`/`part_index`/`part_count`/
// `part_offset` and its own idempotency key, and `Step::Done` comes only after
// the last part was accepted. A part that fails stays the current part, so a
// retry resumes at that part (`begin(..., start_part)`) instead of replaying
// the ones the hub already stored.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "hg/crypto.hpp"
#include "notes_queue.hpp"

namespace hgp::notes {

// True when a read of recording.wav stopped before the size the card reports.
// `fread` returning 0 looks like the end of the file either way, and the
// Sending phase used to take it as one: it sent a body shorter than the
// Content-Length it had already declared, the server sat waiting for the rest,
// and the note only landed after the request timed out and the whole 2.8 MB was
// sent again. It is a failed read, and the upload has to say so and retry.
//
// `ferror` is not the test. The board logged this with the card saying
// `sdmmc_read_blocks failed (0x101)` 60 ms earlier and `ferror` clear: the
// failed read arrives as a short file, so the count is the only signal there
// is. Inline so the host tests can hold the rule without the device HTTP stack.
inline bool audio_read_short(uint32_t read_bytes, uint32_t file_bytes) { return read_bytes != file_bytes; }

// How many bytes one read may take for the part in flight: never past the part's
// own bytes. `fread` alone runs to the end of the recording, so part 0 of a
// 359 s note read 17,247,360 bytes where its share is 14,400,000, the
// short-read rule below fired ("stopped at 17247360 of 14400000 bytes") and no
// request was ever opened (t_666a4273). Inline, like the rules above, so the
// host tests can hold the walk without the device HTTP stack.
inline size_t part_read_len(uint32_t part_bytes, uint32_t done, size_t chunk) {
  if (done >= part_bytes) return 0;
  return std::min<size_t>(chunk, static_cast<size_t>(part_bytes - done));
}

// Where a resumed upload starts. A retry after part N failed must send part N
// and nothing before it — replaying 0..N-1 costs the hub nothing (each is a
// `200 duplicate`) but costs the card the whole note again. A start past the
// plan (a different note, a shorter recording, a part count that changed) is
// not a resume at all: it starts from the beginning rather than sending
// nothing. Inline, like `audio_read_short`, because `Uploader` links
// `esp_http_client` and only the device image can hold the class.
inline size_t resume_index(uint32_t start_part, size_t part_count) {
  return start_part > 0 && start_part < part_count ? static_cast<size_t>(start_part) : size_t(0);
}

// Base64 HMAC-SHA256 of the canonical string with the device key (the core
// keeps the key; see hg::App::sign).
using Signer = std::function<std::string(std::string_view)>;

class Uploader {
 public:
  enum class Step : uint8_t {
    Running,   // work left: call again next pass
    Done,      // 202 accepted or 200 duplicate for the last part: move the note to sent/
    Rejected,  // terminal 4xx: move to failed/
    Retry,     // no answer or a retryable status: stay pending
  };

  ~Uploader();

  // `meta` is copied; `audio_path` is the file to send. `url` is the full
  // endpoint. `now_s` is the request timestamp (X-Gadget-Ts). `access_token`
  // is the hub token, sent as X-Gadget-Token when the hub was provisioned with
  // one (empty otherwise). `start_part` resumes a note at one of its parts (a
  // retry after a part failed); 0 is a note from the beginning.
  bool begin(const Meta& meta, std::string_view audio_path, std::string_view url, int64_t now_s,
             std::string_view access_token, Signer sign, std::string& error,
             uint32_t start_part = 0);
  // One bounded slice of work. Never blocks longer than one 64 KiB transfer
  // (the part-capability probe is one small GET, with its own timeout).
  Step step();
  void cancel();

  int http_status() const { return http_status_; }
  const std::string& error() const { return error_; }
  uint32_t sent() const { return sent_; }        // body bytes written so far (this part)
  uint32_t total() const { return total_; }      // head + audio + tail (this part)
  const Meta& meta() const { return meta_; }
  // The part in flight: 0-based index, how many there are, and whether this
  // note is segmented at all (false = the v1 single POST).
  uint32_t part_index() const { return part_.index; }
  uint32_t part_count() const { return part_.count; }
  bool segmented() const { return parts_.size() > 1; }

 private:
  void close();
  // The POST is over but the note is not: a part that was accepted is followed
  // by the next one, which is read from the same open recording. Closing the
  // whole transfer here left `begin_part()` seeking a NULL file — the unit
  // panicked (LoadProhibited, `fseek` from `begin_part`) right after part 0 was
  // accepted, so no segmented note could ever reach part 1 (t_666a4273).
  void close_transfer();
  bool write_all(const void* data, size_t len);
  // The hub's answer to "do you take parts?" — asked once per boot, and only
  // for a note that needs them. An old hub ignores unknown parts and would
  // store part 0 as the whole note, so a note that needs parts is sent as one
  // POST when the hub does not advertise `note_parts`.
  bool probe_hub_parts();
  // Positions the file at this part and seeds the hasher with its WAV header.
  bool begin_part();
  void degrade_to_single_part();

  enum class Phase : uint8_t { Idle, Probing, Hashing, Sending, Reading, Finished };

  Phase phase_ = Phase::Idle;
  Meta meta_;
  std::string path_;
  std::string url_;
  int64_t now_s_ = 0;
  Signer sign_;
  Request request_;
  FILE* file_ = nullptr;
  void* client_ = nullptr;  // esp_http_client_handle_t
  uint8_t* buf_ = nullptr;   // 64 KiB transfer buffer (heap: the app task's stack is small)
  hg::crypto::Sha256 hasher_;
  std::string token_;
  uint32_t file_bytes_ = 0;
  uint32_t read_ = 0;
  uint32_t sent_ = 0;
  uint32_t total_ = 0;
  bool tail_sent_ = false;
  int http_status_ = 0;
  std::string sha256_;
  std::string error_;

  // Parts (§2.5). `parts_` has one element for a note at or under five
  // minutes, and that one is the legacy whole-file upload.
  WavLayout wav_;
  std::vector<Part> parts_;
  size_t part_at_ = 0;
  Part part_;
  std::string part_header_;   // this part's 44-byte WAV header
  std::string part_sha256_;   // SHA-256 of header + PCM, as sent
  uint32_t part_read_ = 0;    // PCM bytes read for this part
  uint32_t part_sent_ = 0;    // PCM bytes written for this part
  int hub_parts_ = -1;        // -1 unknown, 0 no parts, 1 parts
};

}  // namespace hgp::notes
