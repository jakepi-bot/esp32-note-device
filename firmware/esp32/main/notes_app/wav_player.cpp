// See notes_app/wav_player.hpp: the file half of playback.
#include "notes_app/wav_player.hpp"

#include <cstring>

namespace hgp::notes {
namespace {

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

WavReader::~WavReader() { close(); }

void WavReader::close() {
  if (file_ != nullptr) std::fclose(file_);
  file_ = nullptr;
  played_frames_ = 0;
}

bool WavReader::open(const std::string& path, WavInfo& out) {
  close();
  file_ = std::fopen(path.c_str(), "rb");
  if (file_ == nullptr) return false;

  uint8_t riff[12];
  if (std::fread(riff, 1, sizeof riff, file_) != sizeof riff ||
      std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    close();
    return false;
  }

  WavInfo info;
  bool have_format = false, have_data = false;
  // Chunk walk, so a file with a LIST or fact chunk still plays; ours writes fmt
  // then data and nothing else.
  for (;;) {
    uint8_t header[8];
    if (std::fread(header, 1, sizeof header, file_) != sizeof header) break;
    const uint32_t size = le32(header + 4);
    if (std::memcmp(header, "fmt ", 4) == 0) {
      uint8_t fmt[16] = {};
      const size_t want = size < sizeof fmt ? size : sizeof fmt;
      if (std::fread(fmt, 1, want, file_) != want) break;
      info.channels = le16(fmt + 2);
      info.sample_rate = le32(fmt + 4);
      info.bits_per_sample = le16(fmt + 14);
      have_format = true;
      // Skip anything past the 16 bytes read.
      if (size > want && std::fseek(file_, static_cast<long>(size - want), SEEK_CUR) != 0) break;
      if (size & 1u) std::fseek(file_, 1, SEEK_CUR);  // chunks are word aligned
      continue;
    }
    if (std::memcmp(header, "data", 4) == 0) {
      info.data_bytes = size;
      info.data_offset = static_cast<uint32_t>(std::ftell(file_));
      have_data = true;
      break;  // the payload starts here; nothing after it matters to playback
    }
    // Unknown chunk: skip it whole, word-aligned.
    if (std::fseek(file_, static_cast<long>(size + (size & 1u)), SEEK_CUR) != 0) break;
  }

  if (!have_format || !have_data || info.sample_rate == 0 || info.channels == 0) {
    close();
    return false;
  }
  const uint32_t frame_bytes = info.channels * (info.bits_per_sample / 8u);
  info.duration_s = frame_bytes == 0 ? 0 : info.data_bytes / (frame_bytes * info.sample_rate);
  info_ = info;
  played_frames_ = 0;
  if (std::fseek(file_, static_cast<long>(info.data_offset), SEEK_SET) != 0) {
    close();
    return false;
  }
  out = info_;
  return true;
}

size_t WavReader::read(int16_t* samples, size_t max_frames) {
  if (file_ == nullptr || samples == nullptr || max_frames == 0) return 0;
  // Mono PCM16 is the only shape the notes store writes; a stereo file (never
  // produced here) would be read as its first channel's worth of bytes, which
  // is why the format check above refuses anything else at open() time.
  const size_t want = max_frames * sizeof(int16_t);
  const size_t got = std::fread(samples, 1, want, file_);
  const size_t frames = got / sizeof(int16_t);
  played_frames_ += static_cast<uint32_t>(frames);
  return frames;
}

uint32_t WavReader::position_s() const {
  return info_.sample_rate == 0 ? 0 : played_frames_ / info_.sample_rate;
}

}  // namespace hgp::notes
