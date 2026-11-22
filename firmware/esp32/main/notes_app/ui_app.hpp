// The notes UI's state machine: keys in, one effect and one refresh out.
//
// Two layers, deliberately separate (docs/notes-ui-spec.md §13.1). ui_keys.cpp
// is the pure key map - one press, one Action. This file is the app layer's
// screen machine: it runs the Action, decides which screen is drawn, and says
// what the caller has to do outside the glass (start the codec, erase a note,
// open the radio's window) and how the panel should be refreshed.
//
// It owns no hardware and reads no card: the caller publishes a Snapshot on an
// event and this decides what changed. That is what makes it testable on the
// host with a real notes::Store behind a fake device, and what keeps the draw
// path off the SD card and the radio (§15.4).
#pragma once

#include <cstdint>

#include "notes_app/ui_keys.hpp"
#include "notes_app/ui_render.hpp"
#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

enum class Key : uint8_t { Power, BootTap, BootHold };

// What the caller has to do. Exactly one per press, so a press cannot half-work.
enum class Effect : uint8_t {
  None,
  PowerOff,
  BeginRecording,
  EndRecording,    // stop the capture and commit the note
  DiscardRecording,
  StartPlayback,
  StopPlayback,
  SyncNow,         // connect, upload every pending note, come back to Home
  OpenWifiSetup,   // port_wifi_setup owns the panel until it returns
  OpenPhoneWindow, // port_ble's advertising window owns it until it returns
  ClosePhoneWindow,// the Phone screen's back: the window ends now
  PairingAllow,    // BOOT on the ask: let the phone in (port_ble, then leave)
  PairingRefuse,   // PWR on the ask: reject it (port_ble, then leave)
  DeleteNote,
};

// The refresh class, straight from the spec's transition table (§13.2).
enum class Refresh : uint8_t { None, Partial, Full };

// Which part of the cache changed. The caller knows (it did the work); the app
// only has to answer "does the glass move, and how".
enum Dirty : uint16_t {
  DirtyNone = 0,
  DirtyQueue = 1u << 0,     // counts, card state
  DirtyList = 1u << 1,      // the note rows and their marks
  DirtyLink = 1u << 2,      // Wi-Fi up/down
  DirtyPower = 1u << 3,     // battery percentage, external power, low power
  DirtyClock = 1u << 4,
  DirtyUpload = 1u << 5,    // upload progress or its failure
  DirtyAbout = 1u << 6,     // firmware, device id, card free
  DirtyPlayback = 1u << 7,  // position, duration
  DirtyRecording = 1u << 8, // the counter
};

struct Outcome {
  Effect effect = Effect::None;
  Refresh refresh = Refresh::None;
  bool draw = false;
  uint16_t note = 0;  // DeleteNote / StartPlayback: the index into the snapshot
};

// The stubs the frame about to be drawn will read: [first, first + rows).
// The caller fills exactly this window and nothing else, so the draw path never
// waits on the card (§15.4). It is the app's own answer, not the caller's
// guess, because `frame()` is what decides which indexes it looks at.
struct Window {
  uint8_t first = 0;
  uint8_t rows = 0;
};

class ScreenApp {
 public:
  // Boot: the panel has just been initialised, so the first frame is full.
  void begin(const Snapshot& snapshot);

  // A new cache arrived. Returns whether anything the glass shows changed and,
  // when it did, whether it must be a full refresh: the Home hero changing is
  // the one in-screen change that is always full (§4.3).
  Outcome publish(const Snapshot& snapshot, uint16_t dirty);

  Outcome press(Key key, uint32_t now_ms);

  // The loop's heartbeat: the recording counter, the playback position, and the
  // recording cap. Both counters move at 5 s, never 1 Hz (§9.2).
  Outcome tick(uint32_t now_ms);

  Outcome pairing_ask(uint32_t passkey);
  Outcome pairing_answer(bool allow);
  Outcome playback_finished();

  // The Phone screen's window as the port reports it: the phase, the countdown
  // and the pairing table. Returns the frame when the glass moved - a phase
  // change draws, and so does the countdown once every five seconds, never at
  // 1 Hz (t_9972e2a1).
  Outcome set_phone(const PhoneStatus& status, uint32_t now_ms);

  Screen screen() const { return state_.screen; }
  const State& state() const { return state_; }
  const Snapshot& snapshot() const { return snap_; }
  uint16_t open_note() const { return open_note_; }

  // Everything one frame needs, built from the state and the cache. Allocation
  // free; safe to call from the app task once per flush.
  ScreenDraw frame() const;

  // Which note stubs `frame()` will read. Recompute it after every press: a
  // cursor move rotates the list's window and opening a note changes the index,
  // so a window filled once at boot goes blank as soon as the user scrolls.
  Window window() const;

 private:
  Outcome run(Action action, uint32_t now_ms);
  Outcome stay(Refresh refresh = Refresh::None);
  Outcome go(Screen next, Refresh refresh = Refresh::Full);
  bool hero_changed(const Snapshot& before, const Snapshot& after) const;
  void clamp_cursor();
  void note_changed(uint16_t dirty);

  State state_;
  Snapshot snap_;
  uint16_t open_note_ = 0;
  bool booted_ = false;
  uint8_t bar_moves_ = 0;      // consecutive selection-bar moves (§15.3)
  uint32_t record_start_ms_ = 0;
  uint32_t record_draw_s_ = 0; // the counter's last drawn second
  uint32_t play_draw_s_ = 0;   // the position's last drawn second
  uint32_t passkey_ = 0;
  bool play_done_ = false;
  // The Phone screen's window: what the port last said, and the second the
  // countdown was last drawn for.
  PhoneStatus phone_;
  uint32_t phone_left_s_ = 0;
};

}  // namespace hgp::notesui
