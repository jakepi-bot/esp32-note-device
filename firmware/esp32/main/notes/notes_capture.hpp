// Recording a note: microphone samples from hg::App::on_mic_samples go
// straight into `pending/<note_id>/recording.wav.part` (RIFF, PCM16, mono,
// 24 kHz), the header is patched at the end, and the file is renamed to
// `recording.wav`. The caller then writes note.json, which is what makes the
// note exist (spec §7.2).
//
// Plain stdio: the host test drives it against a temp directory with synthetic
// samples and checks the header, the byte count and the rename.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "notes_queue.hpp"
#include "notes_store.hpp"

namespace hgp::notes {

// The 44-byte canonical WAV header for `data_bytes` of PCM16 mono at 24 kHz.
void wav_header(uint8_t out[44], uint32_t data_bytes, uint32_t sample_rate = kSampleRate);

// How many whole seconds a WAV of `data_bytes` holds (always at least 1 for a
// non-empty recording: the contract's duration_seconds floor).
uint32_t wav_seconds(uint32_t data_bytes, uint32_t sample_rate = kSampleRate);

class Capture {
 public:
  ~Capture();

  // Starts a recording into pending/<id>/. False with `error` set when the
  // note folder or the file cannot be created (no card).
  bool start(Store& store, std::string_view id, uint32_t max_seconds, std::string& error);
  // One microphone buffer (mono PCM16). Silently stops filling at the cap.
  void feed(const int16_t* samples, size_t count);
  // Header patched, file fsynced and renamed. Fills meta.bytes and
  // meta.duration_s. False (with `error`) when the card went away or the
  // recording is unusable, in which case the folder is cleaned up.
  bool finish(Meta& meta, std::string& error);
  // Drops the partial file and the note folder (aborted recording).
  void abort();

  bool active() const { return file_ != nullptr; }
  // The cap was reached: the caller stops the recording now.
  bool full() const { return full_; }
  uint32_t seconds() const { return wav_seconds(bytes_); }
  uint32_t bytes() const { return bytes_; }

 private:
  void close();

  Store* store_ = nullptr;
  std::string id_;
  std::string part_path_;
  std::string final_path_;
  FILE* file_ = nullptr;
  uint32_t bytes_ = 0;       // PCM bytes written
  uint32_t max_bytes_ = 0;   // cap
  bool full_ = false;
  bool failed_ = false;
};

}  // namespace hgp::notes
