// Playing a note back: the file half only.
//
// A note is `pending|sent|failed/<id>/recording.wav`, PCM16 mono at 24 kHz with
// the 44-byte header notes_capture.hpp writes. Reading it is plain stdio, so
// this half runs on the host; the device's playback task (notes_app/main.cpp)
// pumps read() into the ES8311 speaker and publishes the position here, which
// is what the Playing screen shows.
//
// Nothing here allocates a whole note: the caller hands in a fixed buffer, so a
// 300 s note costs the same RAM as a 2 s one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace hgp::notes {

struct WavInfo {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t bits_per_sample = 0;
  uint32_t data_bytes = 0;    // PCM payload
  uint32_t data_offset = 0;   // where the payload starts
  uint32_t duration_s = 0;    // whole seconds; 0 for an empty payload
};

class WavReader {
 public:
  WavReader() = default;
  ~WavReader();
  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;

  // Opens `path` and reads its header. False when the file is missing, not a
  // RIFF/WAVE file, or has no `data` chunk.
  bool open(const std::string& path, WavInfo& out);
  // Up to `max_frames` mono int16 samples. 0 at the end of the payload.
  size_t read(int16_t* samples, size_t max_frames);
  void close();

  bool is_open() const { return file_ != nullptr; }
  uint32_t duration_s() const { return info_.duration_s; }
  // Whole seconds elapsed, counted in samples, so it cannot drift from the audio.
  uint32_t position_s() const;

 private:
  FILE* file_ = nullptr;
  WavInfo info_{};
  uint32_t played_frames_ = 0;
};

}  // namespace hgp::notes
