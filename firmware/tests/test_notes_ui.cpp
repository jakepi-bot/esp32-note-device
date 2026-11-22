// The notes device's screen and key rules (notes_app/ui_keys.hpp), driven the
// way the port drives them: one press, one action. Hardware-free, so the whole
// map is checked here instead of on a desk.
#include "check.hpp"

#include <string>

#include "notes_app/ui_keys.hpp"

using namespace hgp::notesui;

namespace {

State at(Screen screen) {
  State s;
  s.screen = screen;
  return s;
}

}  // namespace

TEST("home: a BOOT tap records, a hold opens the menu, PWR powers off") {
  State s;
  CHECK(press_boot(s) == Action::StartRecording);
  CHECK(press_power(s) == Action::PowerOff);  // PWR is power everywhere but the menu

  CHECK(hold_boot(s) == Action::OpenMenu);
  CHECK(s.screen == Screen::Menu);
  CHECK(s.menu == 0);  // always opens on Notes
}

TEST("recording: BOOT stops and keeps, a hold throws it away, PWR powers off") {
  State s = at(Screen::Recording);
  CHECK(press_boot(s) == Action::StopKeep);
  CHECK(hold_boot(s) == Action::Discard);
  CHECK(press_power(s) == Action::PowerOff);
}

TEST("the menu is the one screen where PWR selects instead of powering off") {
  State s = at(Screen::Menu);
  CHECK(press_power(s) == Action::MenuSelect);
  // BOOT walks the rows and wraps, so one key reaches every item.
  uint8_t seen = 0;
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    CHECK(press_boot(s) == Action::MenuNext);
    seen = s.menu;
  }
  CHECK(seen == 0);  // wrapped back to Notes
  // And one hold always leaves.
  CHECK(hold_boot(s) == Action::Back);
  CHECK(s.screen == Screen::Home);
}

TEST("a notes list walks, opens, and goes back one screen at a time") {
  State s = at(Screen::Menu);
  s.note_count = 3;
  s.screen = Screen::NoteList;
  CHECK(press_boot(s) == Action::NextNote);
  CHECK(s.note == 1);
  CHECK(press_boot(s) == Action::NextNote);
  CHECK(s.note == 2);
  CHECK(press_boot(s) == Action::NextNote);
  CHECK(s.note == 0);  // wraps; three notes and a four-press cycle

  CHECK(press_power(s) == Action::OpenNote);
  CHECK(hold_boot(s) == Action::Back);

  // An empty card opens nothing: the list is the device saying it has no notes.
  State empty = at(Screen::NoteList);
  CHECK(press_power(empty) == Action::None);
  CHECK(press_boot(empty) == Action::None);
}

TEST("a note's own screen offers play, delete and back, and delete asks first") {
  State s = at(Screen::NoteAction);
  CHECK(press_boot(s) == Action::NextNoteAction);
  CHECK(s.action == static_cast<uint8_t>(NoteActionItem::Delete));

  State confirm = at(Screen::ConfirmDelete);
  CHECK(press_boot(confirm) == Action::KeepNote);  // a tap is always the safe answer
  CHECK(press_power(confirm) == Action::KeepNote);
  CHECK(hold_boot(confirm) == Action::DeleteNote);  // the only way to delete
  CHECK(confirm.screen == Screen::NoteList);
}

TEST("playing stops on a BOOT tap or hold, and PWR still powers off") {
  State s = at(Screen::Playing);
  s.playing = true;
  CHECK(press_boot(s) == Action::StopPlay);
  CHECK(!s.playing);
  s.playing = true;
  CHECK(hold_boot(s) == Action::StopPlay);
  s.playing = true;
  CHECK(press_power(s) == Action::PowerOff);
}

TEST("a pairing ask owns both keys and lands back where it interrupted") {
  State s = at(Screen::Menu);
  s.menu = static_cast<uint8_t>(MenuItem::Phone);
  show_pairing_ask(s, Screen::Menu);
  CHECK(s.screen == Screen::Pairing);
  CHECK(press_boot(s) == Action::AllowPhone);
  CHECK(press_power(s) == Action::RefusePhone);
  CHECK(hold_boot(s) == Action::None);  // a hold is never consent
  answer_pairing(s);
  CHECK(s.screen == Screen::Menu);
  CHECK(s.menu == static_cast<uint8_t>(MenuItem::Phone));
}

TEST("every menu row has a label and Back is reachable from the list") {
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    CHECK(menu_label(static_cast<MenuItem>(i))[0] != '\0');
  }
  CHECK(std::string(menu_label(MenuItem::Notes)) == "Notes");
  CHECK(std::string(menu_label(MenuItem::Back)) == "Back");
  CHECK(std::string(action_label(NoteActionItem::Delete)) == "Delete");
}

TEST("the Phone screen: PWR closes the window, a hold goes back, a tap does nothing") {
  State s = at(Screen::Phone);
  CHECK(press_power(s) == Action::ClosePhone);
  CHECK(press_boot(s) == Action::None);
  CHECK(hold_boot(s) == Action::ClosePhone);
  CHECK(s.screen == Screen::Menu);  // back to the row it came from
}
