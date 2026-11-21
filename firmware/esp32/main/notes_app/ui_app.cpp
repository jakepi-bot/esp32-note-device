// See notes_app/ui_app.hpp: the screen machine.
#include "notes_app/ui_app.hpp"

#include <cstdio>

#include "notes_app/strings.hpp"

namespace hgp::notesui {
namespace {

// The four facts that decide Home's hero (§4.2). A change in any of them is the
// one in-screen transition that must be a full refresh: three slots change at
// once and a partial leaves the old word under the new one.
struct HeroFacts {
  bool card;
  bool low_power;
  bool upload_active;
  bool upload_failed;
  bool offline;
  bool clock_set;
};

HeroFacts hero_facts(const Snapshot& s) {
  HeroFacts f{};
  f.card = s.card;
  f.low_power = s.low_power;
  f.upload_active = s.upload_active;
  f.upload_failed = s.upload_failed;
  f.offline = s.count_pending > 0 && !s.wifi_up;
  f.clock_set = s.clock_set;
  return f;
}

bool same(const HeroFacts& a, const HeroFacts& b) {
  return a.card == b.card && a.low_power == b.low_power &&
         a.upload_active == b.upload_active && a.upload_failed == b.upload_failed &&
         a.offline == b.offline && a.clock_set == b.clock_set;
}

}  // namespace

void ScreenApp::begin(const Snapshot& snapshot) {
  snap_ = snapshot;
  state_ = State{};
  state_.note_count = snap_.note_count;
  open_note_ = 0;
  booted_ = true;
  bar_moves_ = 0;
}

void ScreenApp::clamp_cursor() {
  state_.note_count = snap_.note_count;
  if (snap_.note_count == 0) {
    state_.note = 0;
  } else if (state_.note >= snap_.note_count) {
    state_.note = static_cast<uint16_t>(snap_.note_count - 1);
  }
  if (open_note_ >= snap_.note_count) open_note_ = snap_.note_count ? snap_.note_count - 1 : 0;
}

Outcome ScreenApp::publish(const Snapshot& snapshot, uint16_t dirty) {
  Outcome out;
  const HeroFacts before = hero_facts(snap_);
  snap_ = snapshot;
  clamp_cursor();
  const bool hero_moved = !same(before, hero_facts(snap_));

  if (dirty == DirtyNone && !hero_moved) return out;  // nothing on the glass moved

  out.draw = true;
  if (state_.screen == Screen::Home) {
    out.refresh = hero_moved ? Refresh::Full : Refresh::Partial;
  } else if (state_.screen == Screen::NoteList) {
    // A row set or a mark changed: six text rows repaint at once, so the whole
    // page is redrawn. A bar move never arrives here.
    out.refresh = (dirty & (DirtyList | DirtyQueue)) ? Refresh::Full : Refresh::Partial;
  } else {
    out.refresh = Refresh::Partial;
  }
  bar_moves_ = 0;
  return out;
}

Outcome ScreenApp::stay(Refresh refresh) {
  Outcome out;
  out.refresh = refresh;
  out.draw = refresh != Refresh::None;
  return out;
}

Outcome ScreenApp::go(Screen next, Refresh refresh) {
  state_.go(next);
  return stay(refresh);
}

Outcome ScreenApp::run(Action action, uint32_t now_ms) {
  Outcome out;
  switch (action) {
    case Action::None:
      return out;
    case Action::PowerOff:
      out.effect = Effect::PowerOff;
      return out;

    case Action::StartRecording:
      // A device that cannot record says so on Home already (NO CARD); starting
      // a session it cannot finish would only produce a broken note.
      if (!snap_.card) return stay(Refresh::None);
      snap_.recording = true;
      snap_.record_elapsed_s = 0;
      record_start_ms_ = now_ms;
      record_draw_s_ = 0;
      out.effect = Effect::BeginRecording;
      state_.go(Screen::Recording);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::StopKeep:
      snap_.recording = false;
      out.effect = Effect::EndRecording;
      state_.go(Screen::Home);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::Discard:
      snap_.recording = false;
      out.effect = Effect::DiscardRecording;
      state_.go(Screen::Home);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::OpenMenu:
      // ui_keys set the screen and reset the row; the frame is full.
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::MenuNext:
    case Action::NextNoteAction:
      return stay(Refresh::Partial);

    case Action::NextNote:
      // A bar move, unless the list rotated: press() checks that, because only
      // it knows which row was on the glass before the press (§6.5).
      return stay(Refresh::Partial);

    case Action::MenuSelect: {
      const MenuItem item = static_cast<MenuItem>(state_.menu);
      switch (item) {
        case MenuItem::Notes:
          state_.note = 0;
          state_.go(Screen::NoteList);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::SyncNow:
          // Sync runs from Home: the result belongs there, not in the menu.
          state_.go(Screen::Home);
          out.effect = Effect::SyncNow;
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::Phone:
          // port_ble owns the panel for the window; the menu keeps its row.
          out.effect = Effect::OpenPhoneWindow;
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::Wifi:
          out.effect = Effect::OpenWifiSetup;
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::About:
          state_.go(Screen::About);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::Back:
        case MenuItem::Count:
          state_.go(Screen::Home);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
      }
      return out;
    }

    case Action::OpenNote:
      open_note_ = state_.note;
      state_.action = 0;
      state_.go(Screen::NoteAction);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::RunNoteAction:
      switch (static_cast<NoteActionItem>(state_.action)) {
        case NoteActionItem::Play:
          return run(Action::Play, now_ms);
        case NoteActionItem::Delete:
          state_.go(Screen::ConfirmDelete);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case NoteActionItem::Back:
        case NoteActionItem::Count:
          state_.go(Screen::NoteList);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
      }
      return out;

    case Action::AskDelete:
      state_.go(Screen::ConfirmDelete);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::DeleteNote:
      out.effect = Effect::DeleteNote;
      out.note = open_note_;
      state_.go(Screen::NoteList);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::KeepNote:
      state_.go(Screen::NoteList);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::Play:
      if (open_note_ >= snap_.note_count) return stay(Refresh::None);
      state_.playing = true;
      snap_.playing = true;
      snap_.play_pos_s = 0;
      snap_.play_total_s = snap_.notes[open_note_].duration_s;
      play_draw_s_ = 0;
      play_done_ = false;
      out.effect = Effect::StartPlayback;
      out.note = open_note_;
      state_.go(Screen::Playing);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::StopPlay:
      snap_.playing = false;
      out.effect = Effect::StopPlayback;
      state_.go(Screen::NoteAction);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::AllowPhone:
      out.effect = Effect::PairingAllow;
      return out;
    case Action::RefusePhone:
      out.effect = Effect::PairingRefuse;
      return out;

    case Action::Back:
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;
  }
  return out;
}

Outcome ScreenApp::press(Key key, uint32_t now_ms) {
  const Screen before = state_.screen;
  const uint16_t note_before = state_.note;
  const uint8_t first_before = list_visible_first(note_before, snap_.note_count);
  Action action = Action::None;
  switch (key) {
    case Key::Power: action = press_power(state_); break;
    case Key::BootTap: action = press_boot(state_); break;
    case Key::BootHold: action = hold_boot(state_); break;
  }

  Outcome out = run(action, now_ms);

  // ui_keys moves the screen itself for the transitions that are pure key rules
  // (Home hold, a hold out of the list, About, Confirm). Any such move is full.
  if (state_.screen != before) {
    out.refresh = Refresh::Full;
    out.draw = true;
    bar_moves_ = 0;
    return out;
  }

  if (action == Action::MenuNext || action == Action::NextNote ||
      action == Action::NextNoteAction) {
    // Selection-bar moves are boxed: after eight consecutive moves the residue
    // is worth one full flash (§15.3).
    if (state_.screen == Screen::NoteList &&
        list_visible_first(state_.note, snap_.note_count) != first_before) {
      out.refresh = Refresh::Full;
      out.draw = true;
      bar_moves_ = 0;
      return out;
    }
    if (out.refresh == Refresh::Partial) {
      ++bar_moves_;
      if (bar_moves_ >= 8) {
        out.refresh = Refresh::Full;
        bar_moves_ = 0;
      }
    }
  }
  return out;
}

Outcome ScreenApp::tick(uint32_t now_ms) {
  Outcome out;
  if (state_.screen == Screen::Recording) {
    const uint32_t elapsed = (now_ms - record_start_ms_) / 1000;
    if (snap_.note_max_s > 0 && elapsed >= snap_.note_max_s) {
      // The cap reached: the same commit a tap does (§13.2 row 10).
      return run(Action::StopKeep, now_ms);
    }
    // 5 s cadence, driven by a tick count - never a delay in the draw path.
    if (elapsed != record_draw_s_ && elapsed / 5 != record_draw_s_ / 5) {
      record_draw_s_ = elapsed;
      snap_.record_elapsed_s = elapsed;
      out.refresh = Refresh::Partial;
      out.draw = true;
    }
    return out;
  }

  if (state_.screen == Screen::Playing) {
    if (play_done_ || (snap_.play_total_s > 0 && snap_.play_pos_s >= snap_.play_total_s)) {
      return playback_finished();
    }
    if (snap_.play_pos_s / 5 != play_draw_s_ / 5) {
      play_draw_s_ = snap_.play_pos_s;
      out.refresh = Refresh::Partial;
      out.draw = true;
    }
  }
  return out;
}

Outcome ScreenApp::pairing_ask(uint32_t passkey) {
  Outcome out;
  if (!booted_) return out;
  passkey_ = passkey;
  show_pairing_ask(state_, state_.screen);
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Outcome ScreenApp::pairing_answer(bool allow) {
  Outcome out;
  if (state_.screen != Screen::Pairing) return out;
  (void)allow;  // the caller has already accepted or rejected with port_ble
  answer_pairing(state_);
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Outcome ScreenApp::playback_finished() {
  Outcome out;
  if (state_.screen != Screen::Playing) return out;
  play_done_ = false;
  snap_.playing = false;
  out.effect = Effect::StopPlayback;
  state_.go(Screen::NoteAction);
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Window ScreenApp::window() const {
  Window w;
  switch (state_.screen) {
    case Screen::NoteList: {
      // The list's own scroll model decides the top row (§6.4), so ask it
      // rather than recomputing the rotation here.
      const ListView list = build_list(snap_, state_.note);
      if (list.empty) return w;
      w.first = list.first;
      w.rows = kListRows;
      break;
    }
    case Screen::NoteAction:
    case Screen::ConfirmDelete:
    case Screen::Playing:
      // The note's own screen, the confirm and the player all restate the one
      // note they were opened on.
      if (open_note_ > 0xFFu) return w;
      w.first = static_cast<uint8_t>(open_note_);
      w.rows = 1;
      break;
    case Screen::Home:
    case Screen::Back:
      // Only Home's LAST line is a note.
      w.first = 0;
      w.rows = 1;
      break;
    case Screen::Menu:
    case Screen::Recording:
    case Screen::Pairing:
    case Screen::About:
      // Their glass carries counts and copy, never a note's stamp.
      return w;
  }
  if (snap_.note_count == 0 || w.first >= snap_.note_count) return Window{};
  if (static_cast<uint16_t>(w.first) + w.rows > snap_.note_count) {
    w.rows = static_cast<uint8_t>(snap_.note_count - w.first);
  }
  return w;
}

ScreenDraw ScreenApp::frame() const {
  ScreenDraw d;
  d.screen = state_.screen;
  // Home's band is on Recording and Playing too: the queue stays true while a
  // session is open.
  d.home = build_home(snap_);

  switch (state_.screen) {
    case Screen::Home:
    case Screen::Back:
      d.screen = Screen::Home;
      break;
    case Screen::Recording:
      d.recording.elapsed_s = snap_.record_elapsed_s;
      d.recording.cap_s = snap_.note_max_s;
      break;
    case Screen::Menu:
      d.menu = build_menu(snap_, state_.menu);
      break;
    case Screen::NoteList:
      d.list = build_list(snap_, state_.note);
      break;
    case Screen::NoteAction:
      if (!build_note(snap_, open_note_, state_.action, d.note)) {
        // The note went away under us (a phone upload deleted it, a card was
        // pulled): show the list rather than an empty page.
        d.screen = Screen::NoteList;
        d.list = build_list(snap_, state_.note);
      }
      break;
    case Screen::ConfirmDelete:
      if (!build_confirm(snap_, open_note_, d.confirm)) {
        d.screen = Screen::NoteList;
        d.list = build_list(snap_, state_.note);
      }
      break;
    case Screen::Playing: {
      uint32_t duration = snap_.play_total_s;
      if (const NoteStub* note = stub_at(snap_, open_note_)) {
        std::snprintf(d.playing.when, sizeof d.playing.when, "%s %s", note->day, note->clock);
        if (duration == 0) duration = note->duration_s;
      }
      d.playing.elapsed_s = snap_.play_pos_s;
      d.playing.total_s = duration;
      d.playing.bar_pct = duration == 0
                              ? 0
                              : static_cast<uint8_t>((static_cast<uint32_t>(snap_.play_pos_s) * 100u) /
                                                     duration);
      break;
    }
    case Screen::Pairing: {
      // A passkey is six digits by definition; the modulo is belt and braces so
      // the string can never outgrow its eight bytes.
      const uint32_t code = passkey_ % 1000000u;
      std::snprintf(d.pairing.digits, sizeof d.pairing.digits, "%03u %03u",
                    static_cast<unsigned>(code / 1000), static_cast<unsigned>(code % 1000));
      d.pairing.passkey = code;
      break;
    }
    case Screen::About:
      d.about = build_about(snap_);
      break;
  }
  return d;
}

}  // namespace hgp::notesui
