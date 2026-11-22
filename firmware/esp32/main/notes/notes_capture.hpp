// Recording a note: microphone samples from hg::App::on_mic_samples go
// straight into `pending/<note_id>/recording.wav.part` (RIFF, PCM16, mono,
// 24 kHz), the header is patched at the end, and the file is renamed to
// `recording.wav`. The caller then writes note.json, which is what makes the
// note exist (spec §7.2).
//
// The note's length is decided when it starts: the smaller of the setting's
// seconds, the 24 h contract ceiling, and what the card has room for (its free
// space less kNoteReserveBytes, never past kMaxNoteFileBytes). So a note is
// never the write that fills the volume, and a card that fills anyway - another
// writer, a mount that reported more than it had - ends the note with what it
// recorded rather than failing it: a short write is ENOSPC, and finish()
// commits the file the card really holds.
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

// The shortest capture worth keeping: a quarter second of audio. A BOOT tap
// that is released again, or a microphone that delivered nothing, leaves a WAV
// header and a handful of bytes; that is not a note. finish() discards anything
// under this rather than committing it, so a zero-length row can never reach
// pending/ (and the uploader's no-recording sweep never sees a fresh one). The
// number is well past kMinAudioBytes (45), the uploader's own "no audio" line.
constexpr uint32_t kMinNoteMs = 250;
constexpr uint32_t kMinNoteDataBytes =
    kSampleRate * kMinNoteMs / 1000 * kChannels * (kBitsPerSample / 8);

// How many whole seconds a WAV of `data_bytes` holds (always at least 1 for a
// non-empty recording: the contract's duration_seconds floor).
uint32_t wav_seconds(uint32_t data_bytes, uint32_t sample_rate = kSampleRate);

// The audio bytes a note may take on a card with `free_bytes` free: the free
// space less the reserve, held to kMaxNoteFileBytes. 0 means there is no room
// for a note at all. Separate so the arithmetic is one host-tested rule, and so
// the number is the same whoever asks.
uint64_t note_budget_bytes(uint64_t free_bytes);

class Capture {
 public:
  ~Capture();

  // Starts a recording into pending/<id>/, capped by the setting's `max_seconds`
  // and by what the card has room for (asked of the volume the note tree lives
  // on). False with `error` set when the note folder or the file cannot be
  // created (no card), or when the card has no room beyond the reserve: a note
  // that could only fill the volume is not started.
  bool start(Store& store, std::string_view id, uint32_t max_seconds, std::string& error);
  // The same, on a card whose free space the caller already knows (`free_bytes`
  // of 0 = ask the volume). The host tests use this to record against a made-up
  // card; the device uses the first form.
  bool start(Store& store, std::string_view id, uint32_t max_seconds, uint64_t free_bytes,
             std::string& error);
  // One microphone buffer (mono PCM16). Silently stops filling at the cap; a
  // write the card refuses for want of space stops it too, and what landed is
  // kept.
  void feed(const int16_t* samples, size_t count);
  // Header patched with the bytes the file really holds, fsynced and renamed.
  // Fills meta.bytes and meta.duration_s. A capture the card cut short is
  // committed like any other (that is the point: the note exists, with the
  // audio it has). False (with `error`) when the card stopped answering, or
  // when the capture is shorter than kMinNoteDataBytes: that one is discarded -
  // the partial file and the note folder are removed, no note.json is written,
  // and discarded() says why the caller should not treat it as a card fault.
  bool finish(Meta& meta, std::string& error);
  // finish() threw the capture away because it held no audio (under a quarter
  // second). Cleared by the next start().
  bool discarded() const { return discarded_; }
  // Drops the partial file and the note folder (aborted recording).
  void abort();

  bool active() const { return file_ != nullptr; }
  // The cap was reached: the caller stops the recording now.
  bool full() const { return full_; }
  // Why it filled: the card ran out of space mid-write, rather than the budget
  // being spent. Both commit; this is what the caller's log line says.
  bool space_out() const { return space_out_; }
  // The bytes this note may take, and the seconds that works out to: what the
  // recording face shows as its cap. 0 when no capture is running.
  uint64_t budget_bytes() const { return active() ? max_bytes_ : 0; }
  uint32_t cap_seconds() const { return wav_seconds(static_cast<uint32_t>(max_bytes_)); }
  uint32_t seconds() const { return wav_seconds(bytes_); }
  uint32_t bytes() const { return bytes_; }

 private:
  void close();
  // The bytes the file holds, and whether they end on a whole sample.
  uint32_t file_data_bytes() const;

  Store* store_ = nullptr;
  std::string id_;
  std::string part_path_;
  std::string final_path_;
  FILE* file_ = nullptr;
  uint64_t bytes_ = 0;       // PCM bytes written
  uint64_t max_bytes_ = 0;   // cap: the setting, the card, and the ceiling
  bool full_ = false;
  bool space_out_ = false;   // the card filled under the write
  bool failed_ = false;
  bool discarded_ = false;   // finish() found no audio to keep
};

}  // namespace hgp::notes
