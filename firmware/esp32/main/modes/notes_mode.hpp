// Offline voice notes: a PWR tap records to the SD card with no network, the
// finished note joins a durable queue, and the queue uploads itself to Hermes
// whenever the device is online again. See docs/firmware-modules.md and
// docs/waveshare-epaper-154-v2.md §7.
//
// The work is split so the app loop never stalls:
//   modes/notes_mode.cpp   which note is where, the screen, the retry clock
//   notes/notes_queue.*    metadata, ids, multipart framing, response rules
//   notes/notes_store.*    the card tree: pending/sent/failed, atomic commits
//   notes/notes_capture.*  microphone → WAV file
//   notes/notes_uploader.* one queued note → one POST, sliced per loop pass
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "notes/notes_ble.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_queue.hpp"
#include "notes/notes_store.hpp"
#include "notes/notes_uploader.hpp"

namespace hg {
class App;
class AudioIn;
class Power;
}  // namespace hg

namespace hgp {

class DeviceConfig;
class UiState;
struct SdConfig;

// What the status line shows: notes on the card by state. A note counts as
// stored once its files are written, pending until the server acknowledges
// it, synced after that, failed after retries give up.
struct NoteCounts {
  uint32_t stored = 0;   // every note on the card, whatever its state
  uint32_t pending = 0;  // waiting for Hermes
  uint32_t synced = 0;   // acknowledged by the server (in sent/)
  uint32_t failed = 0;   // rejected, kept for inspection (in failed/)
};

// The mode as the e-paper status screen needs it: cached counts and the newest
// note's facts, never a card access, so the screen can ask on every frame.
struct NoteStatus {
  enum class Phase : uint8_t { Idle, Recording, Uploading, Error } phase = Phase::Idle;
  uint32_t recording_s = 0;  // whole seconds recorded so far
  uint32_t pending = 0, synced = 0, failed = 0, stored = 0;
  bool sd_ready = false;
  // The note the uploader holds, while it does.
  bool uploading_valid = false;
  uint32_t uploading_bytes = 0;
  int uploading_hour = 0, uploading_minute = 0;  // local time it was recorded
  // The most recent note on the card (pending or sent), for the resting screen.
  bool has_newest = false;
  bool newest_valid = false;  // it carries a real timestamp (not 1970)
  int newest_hour = 0, newest_minute = 0;
  uint32_t newest_duration_s = 0;
  // Why the mode is in Error (empty otherwise): its own failure, or the last
  // one the live mode put on the notice line.
  std::string error;
};


// The board pieces the mode records through. `mic` is the same AudioIn the
// core's live turns use; the notes mode only opens it when a note records.
struct NotesIo {
  hg::AudioIn* mic = nullptr;
  hg::Power* power = nullptr;  // battery, for note.json (optional)
  SdConfig sd;                 // the card slot
  NoteLink* ble = nullptr;     // the phone's note sync, on boards with Bluetooth
};

class NotesMode {
 public:
  virtual ~NotesMode() = default;
  // Called once after the core has started, with the services the mode
  // shows its state through and the hardware it records on.
  virtual void begin(hg::App& app, UiState& ui, DeviceConfig& config, const NotesIo& io) = 0;
  // A tap on the PWR key: start a recording, or stop the one in progress.
  virtual void on_note_button() = 0;
  // The app loop's heartbeat (every pass, esp_timer milliseconds).
  virtual void tick(uint32_t now_ms) = 0;
  virtual NoteCounts counts() const = 0;
  // The same facts, plus the phase and the newest note: what the status screen
  // draws. Cached; safe to call every frame.
  virtual NoteStatus status() const = 0;
  // The `notes` console command's arguments (empty = status). Returns the same
  // "@ok …" / "@error …" shape the core's commands use, and an empty string
  // for a line it does not know.
  virtual std::string console(std::string_view args) = 0;
};

class NotesModeImpl final : public NotesMode {
 public:
  void begin(hg::App& app, UiState& ui, DeviceConfig& config, const NotesIo& io) override;
  void on_note_button() override;
  void tick(uint32_t now_ms) override;
  NoteCounts counts() const override { return counts_; }
  NoteStatus status() const override;
  std::string console(std::string_view args) override;

 private:
  enum class Mode : uint8_t { Idle, Recording, Syncing };

  void start_note(uint32_t now_ms);
  void finish_note(uint32_t now_ms, const char* why);
  void abort_note(const char* why);
  void begin_sync(uint32_t now_ms);
  void step_sync(uint32_t now_ms);
  void step_ble(uint32_t now_ms);
  void refresh_counts(bool publish);
  void refresh_newest();
  void publish_status();
  bool ensure_sd();
  std::string notes_url() const;
  bool clock_ok() const;
  std::string status_line() const;

  // Bluetooth note sync (notes/notes_ble.hpp): the phone serves itself from the
  // card when there is no Wi-Fi. `ble_` is null on a build without it.
  NoteLink* ble_ = nullptr;
  notes::BleSync ble_sync_;
  std::unique_ptr<notes::BleSource> ble_source_;
  bool ble_linked_ = false;   // the app task has seen the current connection
  unsigned ble_pct_ = 100;    // last progress shown on the screen

  hg::App* app_ = nullptr;
  UiState* ui_ = nullptr;
  DeviceConfig* config_ = nullptr;
  hg::AudioIn* mic_ = nullptr;
  hg::Power* power_ = nullptr;

  notes::Store store_{"/sdcard/notes"};
  notes::Capture capture_;
  notes::Uploader uploader_;
  notes::Meta meta_;  // the note being recorded, or the one being uploaded

  Mode mode_ = Mode::Idle;
  NoteCounts counts_{};
  uint32_t seq_ = 0;
  uint32_t next_scan_ms_ = 0;
  uint32_t queued_bytes_ = 0;   // what the pending notes weigh, for `notes status`
  unsigned last_pct_ = 100;     // last upload percentage shown
  uint32_t rec_started_ms_ = 0; // when the recording now open began

  // The newest note on the card, for the status screen's resting state. Read
  // with the counts (once per kScanMs), never from the screen's own calls.
  std::string newest_id_;
  bool newest_valid_ = false;
  int newest_hour_ = 0, newest_minute_ = 0;
  uint32_t newest_duration_s_ = 0;


  // One note retries at a time; a rejection or a new note moves on.
  std::string sync_id_;
  uint32_t attempt_ = 0;
  uint32_t retry_at_ms_ = 0;
  bool retry_waiting_ = false;  // the backoff stands until retry_at_ms_

  bool sd_ready_ = false;
  uint32_t sd_probe_ms_ = 0;
  std::string sd_error_;
  // Why the last `notes stop` stored nothing (empty: it did). finish_note runs
  // for a button tap and a cap as well, so the console reads this afterwards.
  std::string stop_error_;
  bool formatting_ = false;   // `notes format` runs on its own task: a format
                              // takes tens of seconds and the loop must live
};

}  // namespace hgp
