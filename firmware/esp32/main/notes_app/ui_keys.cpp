// See notes_app/ui_keys.hpp.
#include "notes_app/ui_keys.hpp"

namespace hgp::notesui {
namespace {

uint8_t wrap(uint8_t value, uint8_t count) { return count == 0 ? 0 : static_cast<uint8_t>((value + 1) % count); }

}  // namespace

void show_pairing_ask(State& state, Screen previous) {
  state.after_pairing = previous;
  state.go(Screen::Pairing);
}

void answer_pairing(State& state) { state.go(state.after_pairing); }

Action press_power(State& state) {
  switch (state.screen) {
    case Screen::Pairing:
      // The question is answered on the keys: PWR refuses (the ask's own rule).
      return Action::RefusePhone;
    case Screen::Menu:
      return Action::MenuSelect;  // the menu's row, not the device's power
    case Screen::Phone:
      // PWR is the way out of the Phone screen, and taking it closes the window:
      // a device that keeps advertising with nothing on the glass is a radio
      // left on by accident (t_9972e2a1).
      return Action::ClosePhone;
    case Screen::NoteList:
      return state.note_count == 0 ? Action::None : Action::OpenNote;
    case Screen::NoteAction:
      return Action::RunNoteAction;
    case Screen::ConfirmDelete:
      return Action::KeepNote;
    case Screen::Playing:
      // Stop the audio and leave the player, and never the device: this tap used
      // to power off, so browsing notes switched the board off with one key and
      // did it mid-playback (t_dec6f9ef). BOOT's stop stays on the note's own
      // screen; PWR is the way out, which is the list it came from.
      state.playing = false;
      return Action::StopPlayToList;
    case Screen::Home:
    case Screen::Recording:
    case Screen::About:
    case Screen::Back:
      // A tap is not the power gesture anywhere. Powering off is the hold's, and
      // only from Home (hold_power), so a pocket, a bump or a glance at About
      // cannot end a session.
      return Action::None;
  }
  return Action::None;
}

Action hold_power(State& state) {
  // The one power gesture the screens own. On this board a 2.5 s press also
  // releases the battery latch in hardware whatever the firmware says; what this
  // adds is the time to flush the card, close the radio and drop the latch in
  // order, so a deliberate hold is never a dirty power cut.
  return state.screen == Screen::Home ? Action::PowerOff : Action::None;
}

Action press_boot(State& state) {
  switch (state.screen) {
    case Screen::Pairing:
      return Action::AllowPhone;
    case Screen::Home:
      return Action::StartRecording;
    case Screen::Recording:
      return Action::StopKeep;
    case Screen::Menu:
      state.menu = wrap(state.menu, kMenuCount);
      return Action::MenuNext;
    case Screen::NoteList:
      if (state.note_count == 0) return Action::None;
      state.note = static_cast<uint16_t>((state.note + 1) % state.note_count);
      return Action::NextNote;
    case Screen::NoteAction:
      state.action = wrap(state.action, static_cast<uint8_t>(NoteActionItem::Count));
      return Action::NextNoteAction;
    case Screen::ConfirmDelete:
      // A tap is the safe answer, and it is the same one PWR gives.
      return Action::KeepNote;
    case Screen::Playing:
      state.playing = false;
      return Action::StopPlay;
    case Screen::About:
      return Action::None;  // the page is whole on one screen
    case Screen::Phone:
      return Action::None;  // BOOT does nothing here: the window is PWR's to close
    case Screen::Back:
      return Action::None;
  }
  return Action::None;
}

Action hold_boot(State& state) {
  switch (state.screen) {
    case Screen::Pairing:
      return Action::None;  // a hold never answers a pairing ask
    case Screen::Home:
      state.go(Screen::Menu);
      state.menu = 0;
      return Action::OpenMenu;
    case Screen::Recording:
      return Action::Discard;
    case Screen::Menu:
      state.go(Screen::Home);
      return Action::Back;
    case Screen::NoteList:
      state.go(Screen::Menu);
      return Action::Back;
    case Screen::NoteAction:
      state.go(Screen::NoteList);
      return Action::Back;
    case Screen::ConfirmDelete:
      state.go(Screen::NoteList);
      return Action::DeleteNote;
    case Screen::Playing:
      state.playing = false;
      return Action::StopPlay;
    case Screen::About:
      state.go(Screen::Menu);
      return Action::Back;
    case Screen::Phone:
      // Out the same way the row came in: back to the menu, window closed.
      state.go(Screen::Menu);
      return Action::ClosePhone;
    case Screen::Back:
      return Action::None;
  }
  return Action::None;
}

const char* menu_label(MenuItem item) {
  switch (item) {
    case MenuItem::Notes: return "Notes";
    case MenuItem::SyncNow: return "Sync now";
    case MenuItem::Phone: return "Phone";
    case MenuItem::Wifi: return "Wi-Fi";
    case MenuItem::About: return "About";
    case MenuItem::Back: return "Back";
    case MenuItem::Count: break;
  }
  return "";
}

const char* action_label(NoteActionItem item) {
  switch (item) {
    case NoteActionItem::Play: return "Play";
    case NoteActionItem::Delete: return "Delete";
    case NoteActionItem::Back: return "Back";
    case NoteActionItem::Count: break;
  }
  return "";
}

}  // namespace hgp::notesui
