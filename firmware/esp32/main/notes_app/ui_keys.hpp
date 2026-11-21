// The notes device's screens and keys, as pure state: no GPIO, no panel, no
// clock. The port's app loop feeds it presses and draws whatever it says.
//
// Two keys, one job each. PWR is power everywhere except inside the menu, where
// it selects (it is also the "no" on a delete). BOOT is the work key: a tap
// records, stops, or moves to the next row; a two-second hold opens the menu,
// selects, or deletes, depending on the screen.
//
//   Screen     PWR tap            BOOT tap              BOOT hold
//   Home       power off          start recording       open menu
//   Recording  power off          stop and keep         discard
//   Menu       select the row     next row              back home
//   NoteList   open the note      next note             back to menu
//   NoteAction select the action  next action           back to the list
//   Confirm    keep the note      keep the note         delete the note
//   Playing    power off          stop                  stop
//   Pairing    refuse the phone   allow the phone       (nothing)
//
// Nothing here decides what "select" means for a menu row: the caller maps
// State::menu through MenuItem and runs it, so this stays testable and the
// actions stay in one place.
#pragma once

#include <cstdint>

namespace hgp::notesui {

enum class Screen : uint8_t {
  Home,
  Recording,
  Menu,
  NoteList,
  NoteAction,
  ConfirmDelete,
  Playing,
  Pairing,
  Back,  // internal: "leave this screen", resolved by the caller
};

// The menu, in order. Back is last so one hold always leaves.
enum class MenuItem : uint8_t { Notes, SyncNow, Phone, Wifi, About, Back, Count };

inline constexpr uint8_t kMenuCount = static_cast<uint8_t>(MenuItem::Count);

// What a note's own screen offers.
enum class NoteActionItem : uint8_t { Play, Delete, Back, Count };

// What the caller must do. Exactly one per press.
enum class Action : uint8_t {
  None,
  PowerOff,
  StartRecording,
  StopKeep,
  Discard,
  OpenMenu,
  MenuNext,
  MenuSelect,
  NextNote,
  OpenNote,
  NextNoteAction,
  RunNoteAction,
  AskDelete,
  DeleteNote,
  KeepNote,
  Play,
  StopPlay,
  AllowPhone,
  RefusePhone,
  Back,
};

struct State {
  Screen screen = Screen::Home;
  uint8_t menu = 0;               // MenuItem
  uint8_t action = 0;             // NoteActionItem, while a note is open
  uint16_t note = 0;              // the selected note, newest first
  uint16_t note_count = 0;        // what the card holds
  bool playing = false;
  Screen after_pairing = Screen::Home;  // where the ask lands when answered

  void go(Screen next) { screen = next; }
};

// A phone asking to pair owns both keys while its question is on the glass.
void show_pairing_ask(State& state, Screen previous);
void answer_pairing(State& state);

Action press_power(State& state);
Action press_boot(State& state);
Action hold_boot(State& state);

// The rows, for the renderer. Strings live here so the screen copy has one home.
const char* menu_label(MenuItem item);
const char* action_label(NoteActionItem item);

}  // namespace hgp::notesui
