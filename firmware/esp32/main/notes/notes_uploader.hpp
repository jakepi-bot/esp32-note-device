// The upload half of offline notes: one queued note, one POST to
// `http://<hermes-host>:8766/notes/v1` (contract: docs/voice-note-ingestion.md).
//
// The transfer is sliced: `step()` does at most one 64 KiB read/write and
// returns, so the app loop keeps running its buttons, Wi-Fi and live mode while
// a 14 MB note goes out. Phases: SHA-256 of the audio (the MAC and `checksum`
// need it), then the multipart request, then the answer's classification.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

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

// Base64 HMAC-SHA256 of the canonical string with the device key (the core
// keeps the key; see hg::App::sign).
using Signer = std::function<std::string(std::string_view)>;

class Uploader {
 public:
  enum class Step : uint8_t {
    Running,   // work left: call again next pass
    Done,      // 202 accepted or 200 duplicate: move the note to sent/
    Rejected,  // terminal 4xx: move to failed/
    Retry,     // no answer or a retryable status: stay pending
  };

  ~Uploader();

  // `meta` is copied; `audio_path` is the file to send. `url` is the full
  // endpoint. `now_s` is the request timestamp (X-Gadget-Ts). `access_token`
  // is the hub token, sent as X-Gadget-Token when the hub was provisioned with
  // one (empty otherwise).
  bool begin(const Meta& meta, std::string_view audio_path, std::string_view url, int64_t now_s,
             std::string_view access_token, Signer sign, std::string& error);
  // One bounded slice of work. Never blocks longer than one 64 KiB transfer.
  Step step();
  void cancel();

  int http_status() const { return http_status_; }
  const std::string& error() const { return error_; }
  uint32_t sent() const { return sent_; }        // body bytes written so far
  uint32_t total() const { return total_; }      // head + audio + tail
  const Meta& meta() const { return meta_; }

 private:
  void close();
  bool write_all(const void* data, size_t len);

  enum class Phase : uint8_t { Idle, Hashing, Sending, Reading, Finished };

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
};

}  // namespace hgp::notes
