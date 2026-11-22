// See notes_capture.hpp.
#include "notes_capture.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace hgp::notes {
namespace {

void put_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void put_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

}  // namespace

void wav_header(uint8_t out[44], uint32_t data_bytes, uint32_t sample_rate) {
  std::memcpy(out + 0, "RIFF", 4);
  put_le32(out + 4, 36 + data_bytes);  // the rest of the file after this field
  std::memcpy(out + 8, "WAVE", 4);
  std::memcpy(out + 12, "fmt ", 4);
  put_le32(out + 16, 16);                                       // fmt chunk size
  put_le16(out + 20, 1);                                        // PCM
  put_le16(out + 22, static_cast<uint16_t>(kChannels));         // mono
  put_le32(out + 24, sample_rate);
  put_le32(out + 28, sample_rate * kChannels * (kBitsPerSample / 8));  // byte rate
  put_le16(out + 32, static_cast<uint16_t>(kChannels * (kBitsPerSample / 8)));  // block align
  put_le16(out + 34, static_cast<uint16_t>(kBitsPerSample));
  std::memcpy(out + 36, "data", 4);
  put_le32(out + 40, data_bytes);
}

uint32_t wav_seconds(uint32_t data_bytes, uint32_t sample_rate) {
  const uint32_t bytes_per_second = sample_rate * kChannels * (kBitsPerSample / 8);
  if (data_bytes == 0) return 0;
  const uint32_t whole = data_bytes / bytes_per_second;
  return whole == 0 ? 1 : whole;  // a short note still says "1"
}

Capture::~Capture() { close(); }

void Capture::close() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool Capture::start(Store& store, std::string_view id, uint32_t max_seconds, std::string& error) {
  abort();
  if (id.empty()) {
    error = "empty note id";
    return false;
  }
  if (!store.create_note(id, error)) return false;
  store_ = &store;
  id_.assign(id);
  part_path_ = store.audio_path(NoteState::Pending, id_) + ".part";
  final_path_ = store.audio_path(NoteState::Pending, id_);
  file_ = std::fopen(part_path_.c_str(), "wb");
  if (!file_) {
    error = "cannot open recording.wav";
    store_ = nullptr;
    return false;
  }
  // The cap is the note_max_s setting, and never past kMaxDurationS. The byte
  // budget is computed in 64 bits: 24 h at 24 kHz is 4.1 GB, past a 32-bit
  // product, and the result is then held to kMaxNoteBytes (2 GiB, 12.4 h).
  const uint64_t capped_s = std::min<uint64_t>(max_seconds, kMaxDurationS);
  const uint64_t wanted = capped_s * kSampleRate * kChannels * (kBitsPerSample / 8);
  max_bytes_ = static_cast<uint32_t>(std::min<uint64_t>(wanted, kMaxNoteBytes));
  bytes_ = 0;
  full_ = false;
  failed_ = false;
  uint8_t header[44];
  wav_header(header, 0);
  if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
    error = "cannot write to the card";
    abort();
    return false;
  }
  return true;
}

void Capture::feed(const int16_t* samples, size_t count) {
  if (!file_ || !samples || !count || full_ || failed_) return;
  size_t want = count * sizeof(int16_t);
  if (bytes_ + want >= max_bytes_) {
    want = max_bytes_ - bytes_;  // land exactly on the cap
    want -= want % sizeof(int16_t);
    full_ = true;
  }
  if (want == 0) {
    full_ = true;
    return;
  }
  if (std::fwrite(samples, 1, want, file_) != want) {
    // The card went away mid-note. Keep what is on it and let finish() report
    // the failure: the note is not usable without a complete file.
    failed_ = true;
    return;
  }
  bytes_ += static_cast<uint32_t>(want);
}

bool Capture::finish(Meta& meta, std::string& error) {
  if (!file_) {
    error = "no recording";
    return false;
  }
  const uint32_t data_bytes = bytes_;
  const bool failed = failed_;
  uint8_t header[44];
  wav_header(header, data_bytes);
  bool patched = false;
  if (!failed && std::fseek(file_, 0, SEEK_SET) == 0 &&
      std::fwrite(header, 1, sizeof(header), file_) == sizeof(header)) {
    std::fflush(file_);
    patched = true;
  }
  close();
  if (failed || !patched) {
    error = "the card stopped accepting writes";
    abort();
    return false;
  }
  if (data_bytes == 0) {
    error = "the microphone delivered nothing";
    abort();
    return false;
  }
  if (std::rename(part_path_.c_str(), final_path_.c_str()) != 0) {
    error = "cannot commit recording.wav";
    abort();
    return false;
  }
  meta.bytes = data_bytes;
  meta.duration_s = wav_seconds(data_bytes);
  // The contract's ceiling, in lockstep with kMaxDurationS: a note that ran to
  // the byte cap (12.4 h) or to the 24 h time cap still reports a storable
  // duration_seconds, and nothing here truncates it at 15 min any more.
  if (meta.duration_s > kMaxDurationS) meta.duration_s = kMaxDurationS;
  // The note is the card's now, not this object's. finish() used to leave the
  // committed paths behind, and start() opens with abort() - so the next
  // recording deleted the note recorded before it, which is a note already on
  // the card and, when a tap lands during a sync, the exact file the uploader is
  // reading. An abandoned capture still owns its files; a committed one does not.
  part_path_.clear();
  final_path_.clear();
  id_.clear();
  store_ = nullptr;
  return true;
}

void Capture::abort() {
  close();
  if (!part_path_.empty()) {
    std::remove(part_path_.c_str());
    std::remove(final_path_.c_str());
    if (store_) ::rmdir(store_->note_dir(NoteState::Pending, id_).c_str());
  }
  part_path_.clear();
  final_path_.clear();
  id_.clear();
  store_ = nullptr;
  bytes_ = 0;
  full_ = false;
  failed_ = false;
}

}  // namespace hgp::notes
