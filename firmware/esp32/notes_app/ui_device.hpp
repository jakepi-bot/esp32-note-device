// The app layer's other half: the ports, and the panel.
//
// notes_app/ui_app.cpp decides what a key means and what the glass says;
// this file performs the effects (start the codec, commit or erase a note, open
// the radio's window), keeps the ports' facts and the panel, and renders one
// frame per change. It exists so notes_app/main.cpp stays the boot sequence it
// is today rather than growing a second UI.
//
// The cached snapshot itself (and the rule for when it is rebuilt) is
// notes_app/ui_state.*, which has no ESP-IDF in it: the host tests drive the
// same policy against a real card, which is how the "snapshot built once at boot"
// class of bug (QA D1) became visible to ctest.
//
// The rules it enforces, all from docs/notes-ui-spec.md §15.4:
//   * the draw path touches no hardware but the framebuffer - the snapshot is
//     refreshed on events, never per frame;
//   * nothing waits on the panel: flush() marks it dirty and the driver's own
//     task does the BUSY wait;
//   * only the app task draws, and only the app task reads the card.
//
// Recording is not here: notes_app/runtime.cpp owns the capture, the queue and
// the commit, and the screens hand it the effect (QA D6 - a second, unqueued
// recorder used to live in this class).
#pragma once

#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "battery_curve.hpp"
#include "device_config.hpp"
#include "port_display_epd.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_state.hpp"
#include "notes_app/wav_player.hpp"

namespace hgp {

class NotesUi {
 public:
  // Everything it needs from the board. Pointers, because the ports are
  // singletons owned by app_main.
  struct Ports {
    EpdDisplay* display = nullptr;
    CodecSpeaker* speaker = nullptr;
    LatchPower* power = nullptr;
    PhoneLink* radio = nullptr;
    Wifi* wifi = nullptr;
    TimePort* time = nullptr;
    EspSystem* system = nullptr;
    NvsStorage* storage = nullptr;
    DeviceConfig* config = nullptr;
    notes::Store* store = nullptr;
    // The board's measured discharge curve, when it has one: the band converts
    // the port's mV with the same helper the runtime prints (`notes status`), so
    // the glass and the console cannot disagree about the level.
    hg::Curve battery_curve{};
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

  // The runtime's own capture, asked on every tick and on every runtime change.
  // It is the truth the screens follow: a capture that ends without a key takes
  // the glass off the recording face, and one no key started puts it on. The
  // alternative - the screen believing its own optimistic flag - is what let the
  // glass count a recording the runtime had already committed (console `notes
  // stop`, a microphone error, the card going away).
  struct CaptureProbe {
    std::function<bool()> recording;
    std::function<uint32_t()> elapsed_s;
    // A tap the runtime kept because an upload is in flight: the glass draws the
    // queued face rather than a counter for a note that is not being made.
    std::function<bool()> queued;
  };
  void set_capture_probe(CaptureProbe probe) { capture_probe_ = std::move(probe); }

  // One key, one screen change (§13.2).
  void press(notesui::Key key);

  // The app loop's heartbeat: the counters, the recording cap, playback
  // position, the band's facts, and the panel's redraw request.
  void tick(uint32_t now_ms);

  // A phone asked to pair, and the passkey it negotiated (port_ble's Phone
  // events). The ask is an overlay: it returns where it interrupted.
  void on_pairing_ask();
  void on_pairing_passkey(uint32_t passkey);

  // The runtime's own state moved: a note was committed, a note was deleted or
  // moved, the card came back or went away, retention ran, an upload stepped.
  // The screens cannot see any of it, so this is the publish path QA D1 found
  // missing - without it the glass kept the frame it drew at boot.
  void on_runtime_change();

  // The upload's progress (counts of notes) and its terminal failure.
  void on_upload(bool active, uint8_t done, uint8_t total);
  void on_upload_failed();

  // The console's `ui` command: what the glass is showing, in words.
  std::string console(std::string_view line);

  notesui::Screen screen() const { return state_.screen(); }
  // The screen's own word for where it is ("home", "recording", ...): the first
  // field of `ui`, and the answer `key` prints so a press that started nothing
  // says so at the console without a log hunt.
  const char* screen_word() const;

 private:
  enum class Play : uint8_t { Idle, Starting, Running, Stopping };

  // The ports' answers, read now: the link and the clock every pass, the battery
  // and the card's free space on the slow poll.
  notesui::Facts read_facts() const;
  void poll_facts(uint32_t now_ms, bool slow);
  // The Phone screen's window: the port's phase, clock and table, asked on its
  // own cadence while that screen is up (and never from the draw path).
  notesui::PhoneStatus read_phone() const;
  void poll_phone(uint32_t now_ms);
  void render(notesui::Refresh refresh);
  void apply(const notesui::Outcome& outcome, uint32_t now_ms);
  // Draws the frame an effect left behind - or, when the effect moved the card,
  // the frame a fresh publish builds instead of drawing twice.
  void draw(const notesui::Outcome& outcome, uint32_t now_ms);
  void publish(uint16_t dirty, notesui::Refresh at_least = notesui::Refresh::None);

  void start_playback(uint16_t index);
  void stop_playback();
  static void play_task(void* arg);
  void play_loop();
  // Asks the runtime what it is capturing and hands the answer to the screens.
  // Returns the frame that move owes (Full when the screen changed, none when
  // the two already agree).
  notesui::Outcome sync_capture();

  Ports ports_;
  EffectHook hook_;
  CaptureProbe capture_probe_;
  notesui::UiState state_;
  notesui::Facts facts_;

  std::string device_name_;
  bool begun_ = false;
  bool first_frame_ = true;
  bool in_apply_ = false;
  // A publish the effect asked for while apply() was running: run once the
  // effect's own frame would have been drawn, so a key press repaints once.
  bool publish_pending_ = false;
  uint16_t pending_dirty_ = notesui::DirtyNone;
  uint32_t last_facts_ms_ = 0;
  uint32_t last_slow_facts_ms_ = 0;
  // Every frame this UI has put on the panel since boot. It is the number the
  // flicker budget is measured in (t_3beb371a §5): `ui` prints it, so two
  // readings a minute apart answer "did the glass repaint while nothing
  // happened" without a finger on the board or a log line per refresh.
  uint32_t frames_ = 0;
  // The Phone screen polls the port on its own clock: the window's deadline and
  // a phone connecting are facts the band's 10 s poll should not have to carry.
  uint32_t last_phone_ms_ = 0;
  std::atomic<Play> play_state_{Play::Idle};

  notes::WavReader reader_;
  int16_t* play_buffer_ = nullptr;
  TaskHandle_t play_task_ = nullptr;
  std::atomic<uint32_t> play_pos_s_{0};
  std::atomic<bool> play_stop_{false};
  std::atomic<bool> play_done_{false};
};

}  // namespace hgp
