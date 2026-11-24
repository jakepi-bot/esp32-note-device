// The notes device's screens and keys, as pure state: no GPIO, no panel, no
// clock. The port's app loop feeds it presses and draws whatever it says.
//
// Two keys, one job each. PWR is the way out and the way on: a tap never powers
// the device off (a person browsing notes switched it off with one key, and the
// player did it while audio was playing - t_dec6f9ef). Inside the menu it
// selects, on the player it stops and goes back to the list, and it is the "no"
// on a delete. Only a PWR *hold* from Home powers off. BOOT is the work key: a
// tap records, stops, or moves to the next row; a two-second hold opens the
// menu, selects, or deletes, depending on the screen.
//
//   Screen     PWR tap            PWR hold     BOOT tap           BOOT hold
//   Home       (nothing)          power off    start recording    open menu
//   Recording  (nothing)          (nothing)    stop and keep      discard
//   Menu       select the row     (nothing)    next row           back home
//   NoteList   open the note      (nothing)    next note          back to menu
//   NoteAction select the action  (nothing)    next action        back to the list
//   Confirm    keep the note      (nothing)    keep the note      delete the note
//   ConfirmWipe  cancel, menu     (nothing)    cancel, menu       wipe every note
//   ConfirmFormat cancel, menu    (nothing)    cancel, menu       format the card
//   Wiping     (nothing)          (nothing)    (nothing)          (nothing)
//   UnpairList ask about the phone (nothing)   next phone         back to the menu
//   ConfirmUnpair keep the phone  (nothing)    keep the phone     remove the bond
//   Playing    stop, to the list  (nothing)    stop               stop
//   Pairing    refuse the phone   (nothing)    allow the phone    (nothing)
//   Phone      close the window   (nothing)    (nothing)          back to the menu
//   About      (nothing)          (nothing)    (nothing)          back to menu
//
// Nothing here decides what "select" means for a menu row: the caller maps
// State::menu through MenuItem and runs it, so this stays testable and the
// actions stay in one place.
//
// Wiping is the one screen no key leaves: the confirm has been answered and the
// card work is running off the app task (t_0070a731). A press in the middle of a
// 20-90 s wipe can only confuse, and on this board a 2.5 s PWR hold cuts the
// rail in hardware whatever the firmware decides.
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
  ConfirmWipe,    // empty the device: every note, the card left as filesystem
  ConfirmFormat,  // format the SD card: every note and the card's log
  Wiping,         // the work behind those two confirms, while it runs
  UnpairList,     // the bonded phones, one address a row
  ConfirmUnpair,  // drop one phone's bond: its address is the question
  Playing,
  Pairing,
  Phone,  // the Bluetooth window a person opened from the menu
  About,
  Back,  // internal: "leave this screen", resolved by the caller
};

// The menu, in order. Back is last so one hold always leaves. The three rows
// before it are the destructive ones: wipe every note, format the card, and
// (the follow-on task) drop every paired phone - each behind its own confirm,
// because a hold on the row must never be the thing that empties the device.
enum class MenuItem : uint8_t { Notes, SyncNow, Phone, Wifi, About, WipeNotes, FormatCard, UnpairPhones, Back, Count };

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
  // The two rows that empty the device, and their cancels. A cancel is
  // Action::Back: ui_keys has already put the screen on the menu, and there is
  // nothing outside the glass to undo.
  WipeNotes,
  FormatCard,
  // The Unpair list and its question. The list is the menu's row, and the row
  // the question names is State::peer.
  NextPeer,
  OpenUnpairConfirm,
  UnpairPeer,
  Play,
  StopPlay,         // BOOT: stop and stay on the note's own screen
  StopPlayToList,   // PWR: stop and leave the player for the list
  AllowPhone,
  RefusePhone,
  ClosePhone,  // leave the Phone screen: the window ends with it
  Back,
};

struct State {
  Screen screen = Screen::Home;
  uint8_t menu = 0;               // MenuItem
  uint8_t action = 0;             // NoteActionItem, while a note is open
  uint16_t note = 0;              // the selected note, newest first
  uint16_t note_count = 0;        // what the card holds
  uint8_t peer = 0;               // the selected bonded phone, index into the table
  uint8_t peer_count = 0;         // what the radio's table holds
  bool playing = false;
  // A destructive confirm takes two holds. One hold is also how a person backs
  // out of a screen, and a single hold that wipes the card is how 48 notes went
  // (2026-11-24, my own test's holds). Cleared by any other key and on entry.
  bool confirm_armed = false;
  Screen after_pairing = Screen::Home;  // where the ask lands when answered

  void go(Screen next) { screen = next; }
};

// A phone asking to pair owns both keys while its question is on the glass.
void show_pairing_ask(State& state, Screen previous);
void answer_pairing(State& state);

// A tap: the way out of a screen, never a power-off.
Action press_power(State& state);
// The hold: the only power gesture the screens own, and only from Home. From
// any other screen it is None - the latch's own 2.5 s cutoff still applies in
// hardware, but the firmware does not turn a hold in a submenu into a shutdown.
Action hold_power(State& state);
Action press_boot(State& state);
Action hold_boot(State& state);

// The rows, for the renderer. Strings live here so the screen copy has one home.
const char* menu_label(MenuItem item);
const char* action_label(NoteActionItem item);

}  // namespace hgp::notesui
