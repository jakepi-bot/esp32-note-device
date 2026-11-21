// The app layer's other half: the ports, and the panel.
//
// notes_app/ui_app.cpp decides what a key means and what the glass says;
// this file performs the effects (start the codec, commit or erase a note, open
// the radio's window), keeps the cached snapshot the screens draw from, and
// renders one frame per change. It exists so notes_app/main.cpp stays the boot
// sequence it is today rather than growing a second UI.
//
// The rules it enforces, all from docs/notes-ui-spec.md §15.4:
//   * the draw path touches no hardware but the framebuffer - the snapshot is
//     refreshed on events, never per frame;
//   * nothing waits on the panel: flush() marks it dirty and the driver's own
//     task does the BUSY wait;
//   * only the app task draws, and only the app task reads the card.
#pragma once

#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "device_config.hpp"
#include "port_display_epd.hpp"
#include "notes/capture_writer.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_cache.hpp"
#include "notes_app/wav_player.hpp"

namespace hgp {

class NotesUi {
 public:
  // Everything it needs from the board. Pointers, because the ports are
  // singletons owned by app_main.
  struct Ports {
    EpdDisplay* display = nullptr;
    CodecMic* mic = nullptr;
    CodecSpeaker* speaker = nullptr;
    LatchPower* power = nullptr;
    PhoneLink* radio = nullptr;
    Wifi* wifi = nullptr;
    TimePort* time = nullptr;
    EspSystem* system = nullptr;
    NvsStorage* storage = nullptr;
    DeviceConfig* config = nullptr;
    notes::Store* store = nullptr;
    const char* firmware = "";
  };

  // First frame: full, and Home. Safe to call with a partial port set (no
  // display means the UI runs headless for the console).
  void begin(const Ports& ports);

  // Another module can own an effect - the runtime records, syncs and shuts the
  // device down, so the screens hand it those and do the rest themselves. True
  // means it did the work (or will), and the screens skip their own version.
  using EffectHook = std::function<bool(notesui::Effect, uint16_t note)>;
  void set_effect_hook(EffectHook hook) { hook_ = std::move(hook); }

  // One key, one screen change (§13.2).
  void press(notesui::Key key);

  // The app loop's heartbeat: the counters, the recording cap, playback
  // position, and the panel's redraw request.
  void tick(uint32_t now_ms);

  // Microphone samples while a note records (the Mic event's payload).
  void on_mic_samples(const int16_t* samples, size_t count);

  // A phone asked to pair, and the passkey it negotiated (port_ble's Phone
  // events). The ask is an overlay: it returns where it interrupted.
  void on_pairing_ask();
  void on_pairing_passkey(uint32_t passkey);

  // The uploader published progress (`done` of `total`) or a failed batch.
  void on_upload(bool active, uint8_t done, uint8_t total);
  void on_upload_failed();

  // `Sync now` asked for an upload window: main.cpp's sync code polls this.
  bool take_sync_request();

  // The console's `ui` command: what the glass is showing, in words.
  std::string console(std::string_view line);

  notesui::Screen screen() const { return app_.screen(); }

 private:
  enum class Play : uint8_t { Idle, Starting, Running, Stopping };

  void publish(uint16_t dirty);
  void apply(const notesui::Outcome& outcome, uint32_t now_ms);
  void render(notesui::Refresh refresh);
  void load_counts();
  void load_window();
  void refresh_power();
  std::string device_id();

  void start_recording(uint32_t now_ms);
  void finish_recording(bool keep);
  void start_playback(uint16_t index);
  void stop_playback();
  static void play_task(void* arg);
  void play_loop();

  Ports ports_;
  EffectHook hook_;
  notesui::ScreenApp app_;
  notesui::Snapshot snap_;

  std::string device_id_;
  std::string device_name_;
  notesui::Cache cache_;  // the card as the screens see it, reloaded on events
  std::string note_id_;      // the note being recorded
  uint32_t seq_ = 0;
  bool writer_ready_ = false;
  bool card_ready_ = false;
  bool first_frame_ = true;
  bool sync_requested_ = false;
  std::atomic<Play> play_state_{Play::Idle};

  notes::Capture capture_;
  CaptureWriter writer_;
  notes::WavReader reader_;
  int16_t* play_buffer_ = nullptr;
  TaskHandle_t play_task_ = nullptr;
  std::atomic<uint32_t> play_pos_s_{0};
  std::atomic<bool> play_stop_{false};
  std::atomic<bool> play_done_{false};

  int battery_mv_ = 0;         // the gauge's millivolts, 0 = unknown
  bool uploaded_failed_ = false;
  bool upload_active_ = false;
  uint8_t upload_done_ = 0, upload_total_ = 0;
};

}  // namespace hgp
