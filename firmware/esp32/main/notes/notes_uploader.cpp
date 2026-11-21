// See notes_uploader.hpp.
#include "notes_uploader.hpp"

#include <sys/stat.h>

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "hg/crypto.hpp"
#include "hg/json.hpp"

namespace hgp::notes {
namespace {

const char* TAG = "hg.notes.up";
constexpr size_t kChunk = 64 * 1024;

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
  if (client_) {
    esp_http_client_close(static_cast<esp_http_client_handle_t>(client_));
    esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(client_));
    client_ = nullptr;
  }
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
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
  error_.clear();
}

bool Uploader::begin(const Meta& meta, std::string_view audio_path, std::string_view url, int64_t now_s,
                     std::string_view access_token, Signer sign, std::string& error) {
  cancel();
  if (!sign) {
    error = "no signer";
    return false;
  }
  struct stat st{};
  path_.assign(audio_path);
  if (path_.empty() || ::stat(path_.c_str(), &st) != 0 || st.st_size <= 44) {
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
  // The bytes on the card are what the server sees and what the MAC covers.
  meta_.bytes = static_cast<uint32_t>(st.st_size);
  meta_.sha256.clear();
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
  phase_ = Phase::Hashing;
  return true;
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

  if (phase_ == Phase::Hashing) {
    const size_t n = std::fread(buf_, 1, kChunk, file_);
    if (n > 0) {
      hasher_.update(buf_, n);
      read_ += static_cast<uint32_t>(n);
      return Step::Running;
    }
    if (std::ferror(file_)) {
      phase_ = Phase::Finished;
      error_ = "cannot read recording.wav";
      return Step::Retry;
    }
    std::fseek(file_, 0, SEEK_SET);
    const auto digest = hasher_.finish();
    meta_.sha256 = hg::crypto::hex(digest.data(), digest.size());
    if (!build_request(meta_, "recording.wav", request_)) {
      phase_ = Phase::Finished;
      error_ = "note.json is not uploadable";
      return Step::Rejected;
    }
    const std::string canonical = canonical_string(meta_, now_s_, meta_.sha256);
    const std::string mac = sign_(canonical);
    if (mac.empty()) {
      phase_ = Phase::Finished;
      error_ = "cannot sign the request";
      return Step::Rejected;
    }
    total_ = static_cast<uint32_t>(request_.content_length(file_bytes_));
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
    if (!write_all(request_.head.data(), request_.head.size())) {
      close();
      phase_ = Phase::Finished;
      return Step::Retry;
    }
    phase_ = Phase::Sending;
    return Step::Running;
  }

  if (phase_ == Phase::Sending) {
    const size_t n = std::fread(buf_, 1, kChunk, file_);
    if (n > 0) {
      if (!write_all(buf_, n)) {
        close();
        phase_ = Phase::Finished;
        return Step::Retry;
      }
      return Step::Running;
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
    close();
    phase_ = Phase::Finished;
    const SyncResult result = classify_response(http_status_);
    ESP_LOGI(TAG, "note %s: HTTP %d -> %s", meta_.note_id.c_str(), http_status_,
             result == SyncResult::Accepted    ? "accepted"
             : result == SyncResult::Duplicate ? "duplicate"
             : result == SyncResult::Rejected  ? "rejected"
                                               : "retry");
    switch (result) {
      case SyncResult::Accepted:
      case SyncResult::Duplicate: return Step::Done;
      case SyncResult::Rejected:
        error_ = body.empty() ? "rejected" : body;
        return Step::Rejected;
      case SyncResult::Retry: error_ = body.empty() ? "retry" : body; return Step::Retry;
    }
  }
  return Step::Retry;
}

}  // namespace hgp::notes
