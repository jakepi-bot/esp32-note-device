// See notes_app/ui_app.hpp: the screen machine.
#include "notes_app/ui_app.hpp"

#include <cstdio>
#include <cstring>

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

// ------------------------------------------------------------------ hand-overs
//
// A hand-over is one page replacing another as the person walks the device:
// Home -> menu -> list -> note, and every step back out, plus the questions the
// menu and the list ask on the way (About, Unpair, the two confirms) and the
// note list's own note screen.
//
// They used to ask for a full refresh - a ~1.4 s inversion flash on this panel,
// which is the pause that made the demo path feel unresponsive. The premise was
// that a partial over a page whose ink the next page does not repeat ghosts the
// old layout through the new one. The driver no longer works that way: it diffs
// the frame against what the panel was sent, drives only the rows that moved
// (padded by two), and skips an update that changes no pixel
// (port_display_epd.cpp, update_once). So a hand-over now costs the partial
// waveform over the band that either page inked: where the new page is white,
// the rows the old one inked are re-driven to white in the same pass, and the
// rows that were paper on both pages are never driven at all.
//
// What a partial cannot do is *finish* a black-to-white transition: each one
// leaves a little residue, which is why the panel's kFullEvery exists. A
// hand-over leaves far more than a bar move (a whole layout's ink against a
// whole layout's white), so the driver's ten-band cadence is too lazy for
// them. The machine therefore keeps its own budget, four times tighter: the
// fourth hand-over in a row is a full refresh. A menu session of any depth
// therefore carries at most three pages of residue, and the one flash it sees
// lands on a page the person has just moved to rather than on every key.
constexpr uint8_t kHandoverFullEvery = 4;

// Which screen changes are hand-overs (§15.2). Everything not named here keeps
// the class it had, and each for its own reason:
//   * the first frame after boot - the panel has nothing to diff against
//     (NotesUi::render forces it, and EpdDisplay::begin() arms the full LUT);
//   * the sleeping face - the last image the panel is given (§12.2);
//   * the busy face of a wipe or a format - it stays up for 20-90 s, so one
//     clean flash is nothing against the job, and its progress steps are the
//     partials that follow;
//   * the recording face and the player - the runtime starts and stops the work
//     behind them, and a capture's commit moves the counts under the frame;
//   * the pairing ask - an interrupt from the radio, drawn over whatever page
//     the person was on;
//   * the Phone screen and Wi-Fi setup - the port owns the window or the panel,
//     and the window opening is the radio's news rather than a step the keys
//     took (§12.1);
//   * the Home frame that ends a wipe or a format - the card changed under it.
bool handover(Screen from, Screen to) {
  switch (from) {
    case Screen::Home:
      return to == Screen::Menu;
    case Screen::Menu:
      // The menu's own rows: the note list, About, the bond table, and the two
      // questions. Its Phone and Wi-Fi rows leave the page to a port.
      return to == Screen::Home || to == Screen::NoteList || to == Screen::About ||
             to == Screen::UnpairList || to == Screen::ConfirmWipe ||
             to == Screen::ConfirmFormat;
    case Screen::NoteList:
      return to == Screen::Menu || to == Screen::NoteAction;
    case Screen::NoteAction:
      return to == Screen::NoteList || to == Screen::ConfirmDelete;
    case Screen::ConfirmDelete:
      return to == Screen::NoteList;
    case Screen::ConfirmWipe:
    case Screen::ConfirmFormat:
      return to == Screen::Menu;  // a tap cancels back to the row it came from
    case Screen::About:
      return to == Screen::Menu;
    case Screen::UnpairList:
      return to == Screen::Menu || to == Screen::ConfirmUnpair;
    case Screen::ConfirmUnpair:
      return to == Screen::UnpairList;
    case Screen::Recording:
    case Screen::Playing:
    case Screen::Pairing:
    case Screen::Phone:
    case Screen::Wiping:
    case Screen::Back:
      return false;
  }
  return false;
}

}  // namespace

void ScreenApp::begin(const Snapshot& snapshot) {
  snap_ = snapshot;
  state_ = State{};
  state_.note_count = snap_.note_count;
  // The bonded-phone table is part of the snapshot the machine boots with: the
  // key map reads its count, so it has to be here and not only on the first
  // publish (a BOOT tap on the list would otherwise wrap a table of zero).
  state_.peer_count = snap_.peer_count > kMaxPeerRows ? kMaxPeerRows : snap_.peer_count;
  open_note_ = 0;
  booted_ = true;
  bar_moves_ = 0;
  handovers_ = 0;
  hero_stale_ = false;
}

void ScreenApp::clamp_cursor() {
  state_.note_count = snap_.note_count;
  if (snap_.note_count == 0) {
    state_.note = 0;
  } else if (state_.note >= snap_.note_count) {
    state_.note = static_cast<uint16_t>(snap_.note_count - 1);
  }
  if (open_note_ >= snap_.note_count) open_note_ = snap_.note_count ? snap_.note_count - 1 : 0;
  // The bonded-phone cursor moves with the table the same way: a phone removed
  // by the confirm's hold (or from the console) must not leave the bar past the
  // last row, where the next BOOT tap would wrap from a selection nobody sees.
  state_.peer_count = snap_.peer_count > kMaxPeerRows ? kMaxPeerRows : snap_.peer_count;
  if (state_.peer_count == 0) {
    state_.peer = 0;
  } else if (state_.peer >= state_.peer_count) {
    state_.peer = static_cast<uint8_t>(state_.peer_count - 1);
  }
}

Outcome ScreenApp::publish(const Snapshot& snapshot, uint16_t dirty) {
  Outcome out;
  const HeroFacts before = hero_facts(snap_);
  snap_ = snapshot;
  clamp_cursor();
  const bool hero_moved = !same(before, hero_facts(snap_));
  // A hero that moved while another page is up was not drawn by this frame: the
  // word on the glass is the one Home still shows, and it is the old one. The
  // frame that returns Home has to be full, or the hand-over leaves the previous
  // word ghosted under the new one, which is the artefact §4.3 is about.
  if (hero_moved && state_.screen != Screen::Home) hero_stale_ = true;

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
  // A full frame is the one thing that clears the panel, so it clears the
  // hand-over budget with it: whatever residue was on the glass is gone.
  if (out.refresh == Refresh::Full) handovers_ = 0;
  bar_moves_ = 0;
  return out;
}

// The class a hand-over frame gets (§15.2). The budget is spent one hand-over
// at a time and the one that exhausts it is a full refresh; anything that draws
// full in between resets it, because that flash already cleaned the glass.
Refresh ScreenApp::handover_frame() {
  if (handovers_ + 1 >= kHandoverFullEvery) {
    handovers_ = 0;
    return Refresh::Full;
  }
  ++handovers_;
  return Refresh::Partial;
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
      // Whether a capture can start is the runtime's answer, not this one's: the
      // card may have come back since the last publish, and a device that cannot
      // record says so in the runtime's own words. A decline puts the screen back
      // on Home before the frame is drawn (decline(), through the adapter), so
      // the glass never claims a session that is not happening.
      snap_.recording = true;
      snap_.record_elapsed_s = 0;  // the entry frame is 0:00, full refresh
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
    case Action::OpenUnpairConfirm:
    case Action::NextPeer:
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
          // The row has its own screen now: advertising, waiting, connected and
          // the countdown. It used to hand the window to port_ble and stay on
          // the menu, so a window that failed to open looked like a flash and a
          // step back (t_9972e2a1).
          state_.go(Screen::Phone);
          phone_ = PhoneStatus{};  // advertising, no clock until the port answers
          phone_left_s_ = 0;
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
        case MenuItem::WipeNotes:
          // The ask, never the act: the row opens its confirm and a hold there
          // is what wipes. The count the screen states is the snapshot's own.
          state_.go(Screen::ConfirmWipe);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::FormatCard:
          state_.go(Screen::ConfirmFormat);
          out.refresh = Refresh::Full;
          out.draw = true;
          return out;
        case MenuItem::UnpairPhones:
          // The row's own list (ui_peers.hpp): the phones the radio's bond store
          // holds, one address a row, and a question per phone. It used to be
          // inert; a person with three lost phones had no way out of the table.
          state_.go(Screen::UnpairList);
          state_.peer = 0;
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

    case Action::WipeNotes:
      // ui_keys has put the screen on the busy face, and this effect is the
      // runtime starting the work on its own task (t_0070a731). The frame this
      // Outcome draws is WIPING; the counts arrive on later ticks and Home is
      // drawn when the job ends, from the card the job left.
      card_format_ = false;
      card_done_ = card_total_ = 0;
      card_pct_ = 255;
      out.effect = Effect::WipeNotes;
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::FormatCard:
      card_format_ = true;
      card_done_ = card_total_ = 0;
      card_pct_ = 255;
      out.effect = Effect::FormatCard;
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::UnpairPeer:
      // ui_keys has already put the glass back on the list. The effect forgets
      // this one phone (the index, not the address: the adapter reads the same
      // table the question named), and the publish that follows draws the list
      // one row shorter.
      out.effect = Effect::UnpairPeer;
      out.peer = state_.peer;
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

    case Action::StopPlayToList:
      // The same stop, one screen further out: PWR is the player's way back to
      // the list it came from, the way its BOOT stop goes back to the note.
      snap_.playing = false;
      out.effect = Effect::StopPlayback;
      state_.go(Screen::NoteList);
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::AllowPhone:
      out.effect = Effect::PairingAllow;
      return out;
    case Action::RefusePhone:
      out.effect = Effect::PairingRefuse;
      return out;

    case Action::ClosePhone:
      // Back to the menu, and the window ends with the screen: the port closes
      // it, so the radio is not left advertising behind a page nobody is on.
      state_.go(Screen::Menu);
      out.effect = Effect::ClosePhoneWindow;
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;

    case Action::Back:
      out.refresh = Refresh::Full;
      out.draw = true;
      return out;
  }
  return out;
}

Outcome ScreenApp::press(Key key, uint32_t now_ms) {
  // The busy guard, before ui_keys moves anything: a wipe, a format or an unpair
  // is refused while the device is recording or uploading, and the question stays
  // up with the runtime's reason on it (a recording must not be formatted out from
  // under itself, and a phone must not lose its keys mid-upload). The frame is
  // already showing the reason - the snapshot's busy line was published when it
  // changed - so a refused hold costs no refresh.
  if (key == Key::BootHold &&
      (state_.screen == Screen::ConfirmWipe || state_.screen == Screen::ConfirmFormat ||
       state_.screen == Screen::ConfirmUnpair) &&
      snap_.busy[0] != '\0') {
    return stay(Refresh::None);
  }
  const Screen before = state_.screen;
  const uint16_t note_before = state_.note;
  const uint8_t first_before = list_visible_first(note_before, snap_.note_count);
  Action action = Action::None;
  switch (key) {
    case Key::Power: action = press_power(state_); break;
    case Key::PowerHold: action = hold_power(state_); break;
    case Key::BootTap: action = press_boot(state_); break;
    case Key::BootHold: action = hold_boot(state_); break;
  }

  Outcome out = run(action, now_ms);

  // ui_keys moves the screen itself for the transitions that are pure key rules
  // (Home hold, a hold out of the list, About, Confirm). The class of that move
  // is the machine's answer, not the action's: a hand-over takes the partial
  // path and spends the budget above, and everything else stays full (§15.2).
  if (state_.screen != before) {
    const bool hero_stale = hero_stale_ && state_.screen == Screen::Home;
    out.refresh = (handover(before, state_.screen) && !hero_stale) ? handover_frame() : Refresh::Full;
    if (state_.screen == Screen::Home) hero_stale_ = false;  // this frame is the truth
    out.draw = true;
    bar_moves_ = 0;
  } else if (action == Action::MenuNext || action == Action::NextNote ||
             action == Action::NextNoteAction) {
    // Selection-bar moves are boxed: after eight consecutive moves the residue
    // is worth one full flash (§15.3), and a list rotation (six text rows at
    // once) is not a bar move at all.
    if (state_.screen == Screen::NoteList &&
        list_visible_first(state_.note, snap_.note_count) != first_before) {
      out.refresh = Refresh::Full;
      out.draw = true;
      bar_moves_ = 0;
    } else if (out.refresh == Refresh::Partial) {
      ++bar_moves_;
      if (bar_moves_ >= 8) {
        out.refresh = Refresh::Full;
        bar_moves_ = 0;
      }
    }
  }
  // A full frame clears the glass, so it clears the hand-over budget with it.
  if (out.refresh == Refresh::Full) handovers_ = 0;
  return out;
}

Outcome ScreenApp::tick(uint32_t now_ms) {
  Outcome out;
  if (state_.screen == Screen::Recording) {
    // A queued tap is not a capture: the counter has nothing to count until the
    // runtime starts the note, and the cap cannot arrive before it.
    if (snap_.record_queued) return out;
    const uint32_t elapsed = (now_ms - record_start_ms_) / 1000;
    // A cap commits a note, so the runtime has to agree that the file is that
    // long: the counter's origin can be stale (a tap kept through an upload, a
    // clock that moved under it) and a screen must never stop a capture the card
    // says is empty. The runtime's own second is in `record_elapsed_s`.
    if (snap_.note_max_s > 0 && elapsed >= snap_.note_max_s && snap_.record_elapsed_s + 2 >= snap_.note_max_s) {
      // The cap reached: the same commit a tap does (§13.2 row 10).
      return run(Action::StopKeep, now_ms);
    }
    // 5 s cadence, driven by a tick count - never a delay in the draw path.
    // The entry frame already says 0:00 (run() draws it full), so the first
    // counter the glass sees is the one at five seconds. A minute at a time was
    // too coarse to tell a live capture from a stopped one (Jake, 2026-11-22).
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

void ScreenApp::set_capture_cap(uint32_t cap_s, uint64_t cap_bytes) {
  if (cap_s != 0) snap_.note_max_s = cap_s;
  snap_.record_cap_bytes = cap_bytes;
}

Outcome ScreenApp::follow_capture(bool recording, uint32_t elapsed_s, uint32_t now_ms, bool queued) {
  Outcome out;
  if (queued && !recording) {
    // A tap that landed while an upload was in flight: the runtime kept it and
    // will start the capture when the queue is idle. The glass says so, in the
    // place the counter would be - a person who taps must never be left
    // wondering whether the press registered. Already drawn: no new frame.
    if (snap_.record_queued && state_.screen == Screen::Recording) return out;
    snap_.record_queued = true;
    snap_.recording = false;
    snap_.record_elapsed_s = 0;
    if (state_.screen != Screen::Recording) state_.go(Screen::Recording);
    out.refresh = Refresh::Full;
    out.draw = true;
    return out;
  }
  if (recording && state_.screen == Screen::Home) {
    // A capture no key started: the console's `notes record`. The runtime asks
    // for the glass on purpose (`notify_ui` says so), so Home hands over. Any
    // other screen keeps its place - a person reading the list is not to be
    // yanked into the recording face, and the capture runs either way.
    snap_.recording = true;
    snap_.record_queued = false;
    snap_.record_elapsed_s = elapsed_s;
    record_draw_s_ = elapsed_s;
    record_start_ms_ = now_ms - elapsed_s * 1000u;
    state_.go(Screen::Recording);
    out.refresh = Refresh::Full;
    out.draw = true;
    return out;
  }
  if (recording && state_.screen == Screen::Recording && snap_.record_queued) {
    // The queued tap fired: the face stops saying QUEUED and starts counting,
    // from the runtime's own second so the counter is the length of the file.
    snap_.record_queued = false;
    snap_.recording = true;
    snap_.record_elapsed_s = elapsed_s;
    record_draw_s_ = elapsed_s;
    record_start_ms_ = now_ms - elapsed_s * 1000u;
    out.refresh = Refresh::Full;
    out.draw = true;
    return out;
  }
  if (!recording && state_.screen == Screen::Recording) {
    // The capture ended with no key: a console `notes stop`, a microphone or
    // card error, the runtime's own abort. The glass was counting a recording
    // that no longer exists, which is what a screen state independent of the
    // runtime state costs - the counter ran on over a note already on the card.
    // A queued tap the runtime cancelled lands here too: nothing is being
    // recorded, so the glass goes Home.
    snap_.recording = false;
    snap_.record_queued = false;
    snap_.record_elapsed_s = 0;
    state_.go(Screen::Home);
    out.refresh = Refresh::Full;
    out.draw = true;
  }
  return out;
}

Outcome ScreenApp::decline(Effect effect) {
  Outcome out;
  switch (effect) {
    case Effect::BeginRecording:
      // No capture is running, so the recording face is never painted for it:
      // the entry frame is replaced by Home before it reaches the panel.
      if (state_.screen == Screen::Recording) {
        snap_.recording = false;
        snap_.record_elapsed_s = 0;
        record_start_ms_ = 0;
        record_draw_s_ = 0;
        state_.go(Screen::Home);
        out.refresh = Refresh::Full;
        out.draw = true;
      }
      break;
    case Effect::EndRecording:
    case Effect::DiscardRecording:
      // The runtime was not capturing, so there was nothing to stop: the
      // screens' own move back to Home stands, and the frame it asked for is
      // the one the device is really on.
      break;
    case Effect::WipeNotes:
    case Effect::FormatCard:
      // The runtime would not start the job (busy after the guard passed, or no
      // card): the busy face must not stay up over work that is not happening.
      // Home is where the device really is, and the log says why (t_0070a731).
      if (state_.screen == Screen::Wiping) {
        state_.go(Screen::Home);
        card_pct_ = 255;
        out.refresh = Refresh::Full;
        out.draw = true;
      }
      break;
    default:
      break;
  }
  return out;
}

// The work behind a confirm, as the runtime reports it (t_0070a731). The busy
// face is keyless and the counts only move its bar, so this is where the panel's
// budget is spent: a 5 % step is 20 refreshes for a 114-note wipe, a per-note
// repaint would be 114 x 0.6 s of waveform over a 90 s job.
Outcome ScreenApp::card_job_progress(bool format, uint32_t done, uint32_t total) {
  Outcome out;
  if (state_.screen != Screen::Wiping) return out;
  card_format_ = format;
  const uint8_t pct = total == 0 ? 0 : static_cast<uint8_t>(done > total ? 100u : (done * 100u) / total);
  if (done == card_done_ && total == card_total_) return out;  // nothing new to say
  const bool total_known = total != card_total_;
  const bool stepped = static_cast<int>(pct) >= static_cast<int>(card_pct_) + 5 || done >= total;
  card_done_ = done;
  card_total_ = total;
  if (!total_known && !stepped) return out;
  card_pct_ = pct;
  out.refresh = Refresh::Partial;
  out.draw = true;
  return out;
}

Outcome ScreenApp::card_job_finished() {
  Outcome out;
  // Only the face this machine raised is its to leave: a job reported while the
  // glass is somewhere else (a console-driven wipe on a build without the menu)
  // costs no frame here.
  if (state_.screen != Screen::Wiping) return out;
  state_.go(Screen::Home);
  card_pct_ = 255;
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Outcome ScreenApp::pairing_ask(uint32_t passkey, const uint8_t* peer) {
  Outcome out;
  if (!booted_) return out;
  passkey_ = passkey;
  // A passkey update arrives on the same ask (the screen is already Pairing), so
  // the question keeps the phone that asked it. A fresh ask that names a phone
  // takes that address; one that names none (the host tests, a build without a
  // radio) must not inherit the previous ask's.
  if (peer != nullptr) {
    std::memcpy(ask_peer_, peer, sizeof ask_peer_);
    ask_peer_known_ = true;
  } else if (state_.screen != Screen::Pairing) {
    ask_peer_known_ = false;
  }
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
  ask_peer_known_ = false;
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Outcome ScreenApp::pairing_paired(const uint8_t* peer) {
  Outcome out;
  // Only the ask on the glass, and only the phone that asked it. A pair that
  // completes while a *second* phone's question is up (the first answer given on
  // the console, the second ask landing before the first bond) is not an answer
  // to the second phone: that question stays until its own keys decide it.
  if (state_.screen != Screen::Pairing) return out;
  if (ask_peer_known_ && peer != nullptr && std::memcmp(ask_peer_, peer, sizeof ask_peer_) != 0) return out;
  answer_pairing(state_);
  ask_peer_known_ = false;
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

// The Phone screen's window, as the port last reported it. The screen owns what
// the glass shows; the port owns the radio, so this turns the port's answer into
// a frame and nothing else.
Outcome ScreenApp::set_phone(const PhoneStatus& status, uint32_t now_ms) {
  (void)now_ms;
  Outcome out;
  const PhoneStatus before = phone_;
  phone_ = status;
  if (state_.screen != Screen::Phone) return out;  // only the Phone screen draws it
  // The word and the table are facts that change on an event; the countdown is
  // the only thing that moves by itself, and it moves on the 5 s beat the
  // recording counter keeps rather than at 1 Hz (§9.2).
  const bool word_moved = before.phase != status.phase || before.refusal != status.refusal;
  const bool table_moved = before.paired != status.paired || before.slots != status.slots;
  const uint32_t left_s = status.left_ms / 1000;
  // `before.left_ms == 0` is the window just opening (or reopening): the clock
  // has to be drawn once, then only when the beat moves.
  const bool countdown_moved =
      status.left_ms != 0 && (before.left_ms == 0 || left_s / 5 != phone_left_s_ / 5);
  phone_left_s_ = left_s;
  if (!word_moved && !table_moved && !countdown_moved) return out;
  out.draw = true;
  out.refresh = (word_moved || table_moved) ? Refresh::Full : Refresh::Partial;
  return out;
}

Outcome ScreenApp::playback_finished() {
  Outcome out;
  play_done_ = false;
  snap_.playing = false;
  out.effect = Effect::StopPlayback;
  state_.go(Screen::NoteAction);
  out.refresh = Refresh::Full;
  out.draw = true;
  return out;
}

Outcome ScreenApp::sleep_face() const {
  // No state moves: the machine stays on the screen it was showing, because
  // nothing will ask it for another frame. The answer is the frame itself and
  // the waveform it needs - the last full update this panel gets, which is also
  // the only way to leave no trace of the page it replaces.
  Outcome out;
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
    case Screen::Phone:
    case Screen::About:
    case Screen::ConfirmWipe:
    case Screen::ConfirmFormat:
    case Screen::Wiping:
    case Screen::UnpairList:
    case Screen::ConfirmUnpair:
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
      d.recording.cap_bytes = snap_.record_cap_bytes;
      d.recording.queued = snap_.record_queued;
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
    case Screen::ConfirmWipe:
      d.confirm = build_wipe_confirm(snap_);
      break;
    case Screen::ConfirmFormat:
      d.confirm = build_format_confirm(snap_);
      break;
    case Screen::Wiping:
      d.busy = build_busy(card_format_, card_done_, card_total_);
      break;
    case Screen::UnpairList:
      d.unpair = build_unpair(snap_, state_.peer);
      break;
    case Screen::ConfirmUnpair:
      // The phone this question is about. If the table lost it between the press
      // and the frame (the console's `phone forget`), the list is the honest
      // page - the question would otherwise name nobody.
      if (!build_unpair_confirm(snap_, state_.peer, d.confirm)) {
        d.screen = Screen::UnpairList;
        d.unpair = build_unpair(snap_, state_.peer);
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
    case Screen::Phone:
      d.phone = build_phone(snap_, phone_);
      break;
  }
  return d;
}

}  // namespace hgp::notesui
