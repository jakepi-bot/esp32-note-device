// Sends one offline note exactly the way the firmware does: the same
// notes::Request framing, the same canonical string, the same HMAC, the same
// Content-Length. The only difference is the transport (a plain socket here,
// esp_http_client on the board), so running this against a live
// `plugin.notes_ingest` server verifies the device's contract half without the
// device needing Wi-Fi.
//
//   hg_notes_request --url http://127.0.0.1:8766/notes/v1 \
//     --key <base64 device key> --device-id hg-… --audio recording.wav \
//     --at 2026-11-20T02:05:12Z --duration 8 [--repeat] [--token T] [--upload-time]
//
// `--upload-time` stamps the way the board does when it never had a clock: a
// 1970 `--at` is replaced by the send time (notes::stamped_for_upload), so the
// vault note is dated when it arrived rather than 1970.
//
// Exit code 0 when the first answer is 202/200 (and, with --repeat, the second
// is 200 duplicate).
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "hg/crypto.hpp"
#include "notes/notes_queue.hpp"

namespace {

struct Args {
  std::string url;
  std::string key;
  std::string device_id = "hg-0123456789abcdef";
  std::string note_id;
  std::string audio;
  std::string recorded_at;
  std::string token;
  uint32_t duration = 1;
  int64_t ts = 0;
  bool repeat = false;
  bool upload_time = false;  // stamp the way the board does when it never had a clock
};

bool parse(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (arg == "--url") {
      if (!next(a.url)) return false;
    } else if (arg == "--key") {
      if (!next(a.key)) return false;
    } else if (arg == "--device-id") {
      if (!next(a.device_id)) return false;
    } else if (arg == "--note-id") {
      if (!next(a.note_id)) return false;
    } else if (arg == "--audio") {
      if (!next(a.audio)) return false;
    } else if (arg == "--at") {
      if (!next(a.recorded_at)) return false;
    } else if (arg == "--token") {
      if (!next(a.token)) return false;
    } else if (arg == "--duration") {
      std::string v;
      if (!next(v)) return false;
      a.duration = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (arg == "--ts") {
      std::string v;
      if (!next(v)) return false;
      a.ts = std::strtoll(v.c_str(), nullptr, 10);
    } else if (arg == "--repeat") {
      a.repeat = true;
    } else if (arg == "--upload-time") {
      a.upload_time = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return false;
    }
  }
  if (a.url.empty() || a.key.empty() || a.audio.empty() || a.recorded_at.empty()) {
    std::fprintf(stderr, "usage: hg_notes_request --url URL --key B64 --audio FILE --at ISO [--device-id ID] "
                         "[--note-id ID] [--duration N] [--ts UNIX] [--token T] [--repeat] [--upload-time]\n");
    return false;
  }
  if (a.ts == 0) a.ts = static_cast<int64_t>(std::time(nullptr));
  return true;
}

// Minimal http:// parser: host, port, path.
bool split_url(const std::string& url, std::string& host, std::string& port, std::string& path) {
  const std::string scheme = "http://";
  if (url.rfind(scheme, 0) != 0) return false;
  std::string rest = url.substr(scheme.size());
  const size_t slash = rest.find('/');
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (slash != std::string::npos) rest.resize(slash);
  const size_t colon = rest.rfind(':');
  if (colon == std::string::npos) {
    host = rest;
    port = "80";
  } else {
    host = rest.substr(0, colon);
    port = rest.substr(colon + 1);
  }
  return !host.empty();
}

bool send_all(int fd, const void* data, size_t len) {
  const auto* p = static_cast<const char*>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

// Returns the HTTP status (0 on transport failure) and fills `body`.
int post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
         const std::string& head, const std::string& audio, const std::string& tail, std::string& body) {
  std::string host, port, path;
  if (!split_url(url, host, port, path)) return 0;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return 0;
  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    return 0;
  }
  if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    freeaddrinfo(res);
    ::close(fd);
    return 0;
  }
  freeaddrinfo(res);

  std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + port + "\r\n";
  for (const auto& h : headers) request += h.first + ": " + h.second + "\r\n";
  request += "Content-Length: " + std::to_string(head.size() + audio.size() + tail.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";

  if (!send_all(fd, request.data(), request.size()) || !send_all(fd, head.data(), head.size()) ||
      !send_all(fd, audio.data(), audio.size()) || !send_all(fd, tail.data(), tail.size())) {
    ::close(fd);
    return 0;
  }
  std::string response;
  char buf[2048];
  ssize_t n = 0;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) response.append(buf, static_cast<size_t>(n));
  ::close(fd);

  int status = 0;
  if (response.rfind("HTTP/1.", 0) == 0) status = std::atoi(response.c_str() + 9);
  const size_t split = response.find("\r\n\r\n");
  body = split == std::string::npos ? std::string() : response.substr(split + 4);
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) return 2;

  // The audio as it sits in pending/<note_id>/recording.wav.
  std::string audio;
  if (FILE* f = std::fopen(a.audio.c_str(), "rb")) {
    char buf[64 * 1024];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) audio.append(buf, n);
    std::fclose(f);
  }
  if (audio.size() <= 44) {
    std::fprintf(stderr, "cannot read %s\n", a.audio.c_str());
    return 2;
  }

  std::vector<uint8_t> key;
  if (!hg::crypto::base64_decode(a.key, key) || key.empty()) {
    std::fprintf(stderr, "the device key is not base64\n");
    return 2;
  }

  hgp::notes::Meta meta;
  meta.device_id = a.device_id;
  meta.note_id = a.note_id.empty()
                     ? hgp::notes::make_note_id(a.device_id, a.ts, 1)
                     : a.note_id;
  meta.idempotency_key = meta.note_id;
  meta.device_name = "EPaper";
  meta.firmware = "host-tool";
  meta.created_ms = a.ts * 1000;
  meta.recorded_at = a.recorded_at;
  meta.duration_s = a.duration;
  meta.bytes = static_cast<uint32_t>(audio.size());
  meta.sha256 = hgp::notes::sha256_hex(reinterpret_cast<const uint8_t*>(audio.data()), audio.size());

  // --upload-time: the board has no RTC, so a note taken before its first clock
  // sync carries a 1970 stamp and is uploaded stamped with the time it is sent
  // (notes::stamped_for_upload). Same function, same result as on the board.
  if (a.upload_time) {
    bool restamped = false;
    meta = hgp::notes::stamped_for_upload(meta, a.ts, &restamped);
    if (restamped) {
      std::printf("recorded_at %s is not a time (the board had no clock yet): uploading it as %s\n",
                  a.recorded_at.c_str(), meta.recorded_at.c_str());
    }
  }

  hgp::notes::Request request;
  if (!hgp::notes::build_request(meta, "recording.wav", request)) {
    std::fprintf(stderr, "the note metadata is not uploadable\n");
    return 2;
  }
  const std::string canonical = hgp::notes::canonical_string(meta, a.ts, meta.sha256);
  const auto mac = hg::crypto::hmac_sha256(key.data(), key.size(),
                                           reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Content-Type", request.content_type()},
      {"X-Gadget-Ts", std::to_string(a.ts)},
      {"X-Gadget-Mac", hg::crypto::base64_encode(mac.data(), mac.size())},
  };
  if (!a.token.empty()) headers.push_back({"X-Gadget-Token", a.token});

  std::printf("note %s: %zu audio bytes, request %zu bytes\n", meta.note_id.c_str(), audio.size(),
              request.content_length(audio.size()));
  std::string body;
  const int first = post(a.url, headers, request.head, audio, request.tail, body);
  std::printf("first  -> HTTP %d %s\n", first, body.c_str());
  if (first != 202 && first != 200) return 1;
  if (!a.repeat) return 0;

  body.clear();
  const int second = post(a.url, headers, request.head, audio, request.tail, body);
  std::printf("repeat -> HTTP %d %s\n", second, body.c_str());
  return second == 200 && body.find("\"duplicate\"") != std::string::npos ? 0 : 1;
}
