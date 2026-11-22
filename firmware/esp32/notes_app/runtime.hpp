// The notes-only app's runtime: everything the device does that is not a screen.
//
// Its own module rather than the mode router's: hg::App is gone from this app
// (docs/notes-only-firmware-plan.md, section 3, option B), and modes/notes_mode
// is written against it. What is kept is the part that decides — the card tree,
// the recording, the upload planning and its HMAC, the Bluetooth transfer and
// the retention rule — over the same modules the Hermes app uses.
//
// Three pieces of policy live here and nowhere else:
//   * power: the battery latch, the percentage, and a shutdown that flushes the
//     card and drops the latch in that order;
//   * the radio's duty cycle: Wi-Fi is associated only inside a window (plan
//     section 4), and the Bluetooth window is the port's own (hg/radio_window);
//   * the clock: one SNTP window at boot and whenever the clock is unset.
//
// Playback is not here: it needs the speaker's writer task and the note list,
// and it lands with the screens.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "freertos/FreeRTOS.h"

#include "notes/capture_writer.hpp"
#include "notes/notes_ble.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_queue.hpp"
#include "notes/notes_retention.hpp"
#include "notes/notes_store.hpp"
#include "notes/notes_uploader.hpp"

namespace hg {
class AudioIn;
class Storage;
struct Curve;
}  // namespace hg

namespace hgp {
class DeviceConfig;
class LatchPower;
class NoteLink;
class TimePort;
class Wifi;
struct BoardConfig;
struct SdConfig;
}  // namespace hgp

namespace hgn {

// The notes stack's own namespace, spelled short: this module is written
// against it, and every type below is hgp::notes'.
namespace notes = hgp::notes;

// What the status face draws, as the runtime knows it. Cached: the screen may
// ask on every frame, and a card scan is not a frame's work.
struct Status {
  enum class Phase : uint8_t { Idle, Recording, Uploading, Error };
  Phase phase = Phase::Idle;
  uint32_t recording_s = 0;
  uint32_t pending = 0, sent = 0, failed = 0, stored = 0;
  bool sd_ready = false;
  bool clock_set = false;
  int battery_pct = -1;  // -1: not measured
  uint16_t battery_mv = 0;
  bool charging = false, external_power = false;
  std::string wifi;    // the station's SSID while a window holds a link
  bool wifi_window = false;
  std::string error;
};

class Runtime {
 public:
  // The board pieces the runtime drives. Nothing here is optional except the
  // battery (a board without the sense) and the phone link (no NimBLE).
  struct Io {
    hg::AudioIn* mic = nullptr;
    hgp::LatchPower* power = nullptr;
    hgp::NoteLink* ble = nullptr;
    hgp::Wifi* wifi = nullptr;
    hgp::TimePort* time = nullptr;
    hgp::DeviceConfig* config = nullptr;
    hg::Storage* storage = nullptr;
    hgp::SdConfig sd{};
    hg::Curve battery_curve{};
    const char* board_name = "";
  };

  // Mounts the card if it answers, loads (or makes) the device key, starts the
  // writer and the Bluetooth queue adapter. False when there is no storage to
  // read the identity from: the device then has nothing to upload with.
  bool begin(const Io& io);

  // What the glass may need to know. The runtime owns the recording, the queue,
  // the card and the upload; the screens cache all four, so every change is
  // published here. These were missing entirely (QA D1), which is why the glass
  // kept the frame it drew at boot: a note recorded after power-on, an upload, a
  // delete and a lost card never reached it.
  struct UiHooks {
    std::function<void()> changed;                              // counts, list or card state
    std::function<void(bool, uint8_t, uint8_t)> upload;         // active, done, total
    std::function<void()> upload_failed;                        // the note moved to failed/
  };
  void set_ui_hooks(UiHooks hooks) { ui_ = std::move(hooks); }

  // The app loop's pass. `wifi_link` is whether a window currently holds an
  // association (Wifi::current() is not empty): the upload and the SNTP sync
  // only run while it does.
  void tick(uint32_t now_ms, bool wifi_link);

  // Microphone samples (EventType::Mic). Copied into the writer's ring; the
  // card write happens on the writer's own task.
  void on_mic(const int16_t* samples, size_t count);

  // The SNTP callback posted TimeSynced: write the RTC and stop asking.
  void on_clock_synced();

  // A key action from ui_keys: StartRecording, StopKeep, Discard, DeleteNote,
  // SyncNow (the menu row) and PowerOff are the ones this module runs. Returns
  // what it did, for the caller's log line; empty when the action is not its.
  std::string on_action(int action);  // hgp::notesui::Action as an int on purpose

  // The radio's duty cycle. wants_network() is what the app loop asks before
  // touching the station; open_window(Net) is a person asking (`Sync now`, the
  // Wi-Fi row) and opens one at once.
  bool wants_network(uint32_t now_ms) const { return net_open(now_ms); }
  void sync_now(uint32_t now_ms);
  const char* net_reason() const;
  // The radio's standing choice, kept in NVS (`wifi`, on by default): on means
  // the station stays associated after boot instead of closing every window, off
  // is the duty cycle below. The last choice survives a power cycle.
  bool wifi_stay() const { return wifi_stay_; }
  void set_wifi_stay(bool on);
  // The setup page a phone opened: the window must not close under it.
  bool setup_window() const;

  // Flush the card, close the radio and drop the latch. Recording in progress
  // is finished, not dropped: its note.json is the commit. On USB the latch
  // stays up, so the card is mounted again and the device carries on.
  void shutdown();

  Status status(uint32_t now_ms) const;
  std::string console(std::string_view args);
  bool recording() const { return mode_ == Mode::Recording; }
  // A BOOT tap made while an upload was in flight: kept, not dropped, and armed
  // to start the capture on the pass the upload ends. The glass draws it.
  bool recording_queued() const { return pending_record_; }
  // The capture's own elapsed second, read off the WAV the writer has filled.
  // The screens adopt it as their origin when the runtime - not a key - started
  // the capture (`notes record`), so the counter on the glass is the length of
  // the file and not the time since the glass noticed.
  uint32_t record_elapsed_s() const { return capture_.seconds(); }

 private:
  enum class Mode : uint8_t { Idle, Recording, Syncing };
  // Why a Wi-Fi window is open. Every reason is bounded; Retry is the one that
  // reopens itself, on its own slow clock, while a note waits or the clock is
  // still unset (a device that has been offline for an hour must not need a
  // reboot to upload what it recorded).
  // Retry is the one that reopens itself, on its own slow clock, while a note
  // waits or the clock is still unset: a device that has been offline for an
  // hour must not need a reboot to upload what it recorded.
  enum class Net : uint8_t { Closed, Boot, Note, Manual, Clock, Retry, Stay };

  bool load_identity();
  void start_note(uint32_t now);
  void finish_note(uint32_t now, const char* why);
  void abort_note(const char* why);
  void begin_sync(uint32_t now);
  void step_sync(uint32_t now);
  void step_ble(uint32_t now);
  // The window's own clock and its close, split out of tick() so a recording
  // cannot defer them (QA D4).
  void step_window(uint32_t now);
  void run_retention(uint32_t now, bool force);
  bool ensure_sd();
  void refresh_counts();
  void refresh_battery();
  // The glass's side of a state change: one publish, no state of its own.
  void notify_ui();
  void notify_upload();
  std::string notes_url() const;
  std::string sign(std::string_view canonical) const;

  void open_net(uint32_t now, Net why);
  bool net_open(uint32_t now) const;
  void close_net(uint32_t now);

  Io io_;
  notes::Store store_;
  notes::Capture capture_;
  hgp::CaptureWriter writer_;
  notes::Uploader uploader_;
  std::unique_ptr<notes::BleSource> ble_source_;
  notes::BleSync ble_sync_;
  notes::RetentionPolicy retention_;
  notes::RetentionResult retention_last_;

  Mode mode_ = Mode::Idle;
  // The tap that arrived during a sync: armed, drawn on the glass, and fired on
  // the first pass the queue is idle again.
  bool pending_record_ = false;
  notes::Meta meta_;
  std::string sync_id_;
  uint32_t attempt_ = 0;
  bool retry_waiting_ = false;
  uint32_t retry_at_ms_ = 0;
  std::string stop_error_;
  bool writer_ready_ = false;  // the writer task exists; else the recording writes inline
  bool ble_linked_ = false;

  uint32_t pending_ = 0, sent_ = 0, failed_ = 0;
  // The upload batch the glass draws: notes this window has sent and how many it
  // set out to send. The counters are the screen's progress bar, nothing else.
  bool batch_running_ = false;
  uint8_t batch_done_ = 0, batch_total_ = 0;
  UiHooks ui_;
  bool sd_ready_ = false;
  std::string sd_error_;
  uint32_t sd_probe_ms_ = 0;
  uint8_t sd_probes_ = 0;  // attempts since boot: the first ones retry quickly
  uint32_t next_scan_ms_ = 0, next_retention_ms_ = 0, next_battery_ms_ = 0;
  bool low_space_ = false;

  int battery_pct_ = -1;
  uint16_t battery_mv_ = 0;
  bool charging_ = false, external_ = false;
  bool charging_known_ = false;  // the board has a charge signal to read

  std::vector<uint8_t> key_;  // 32 bytes; the HMAC's key, never logged
  std::string device_id_, device_name_, token_;

  Net net_ = Net::Closed;
  uint32_t net_until_ms_ = 0;
  uint32_t next_net_retry_ms_ = 0;  // the slow clock a closed window comes back on
  bool net_was_up_ = false;  // an association held during this window
  bool wifi_stay_ = true;    // the `wifi` setting: stay associated (on) or duty-cycle (off)
  // Milliseconds the Wi-Fi window has been open since boot: the duty cycle's up
  // half, which is what `notes status` reports (the Bluetooth window is the
  // port's own and is bounded by hg::RadioWindow).
  uint32_t radio_ms_ = 0;
  uint32_t last_tick_ms_ = 0;
  std::string error_;
  bool ready_ = false;
};

}  // namespace hgn
