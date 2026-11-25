// See notes_app/ui_device.hpp.
#include "ui_device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "hg/crypto.hpp"
#include "hg/protocol.hpp"
#include "notes/notes_retention.hpp"
#include "notes/sd_mount.hpp"
#include "notes_app/battery_facts.hpp"
#include "notes_app/strings.hpp"
#include "notes_app/ui_render.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.ui";

// Playback reads 100 ms at a time: small enough that a stop lands within a
// frame, large enough that the card is not asked for 77 tiny reads a second.
constexpr size_t kPlayFrames = 2400;
// While a note plays, the reader keeps one buffer in the speaker and waits.
constexpr uint32_t kPlayPaceMs = 20;
// The band's facts are a string compare and a clock read: ten seconds is the
// policy (t_3beb371a §5) - a link that came up lands on the glass within a poll,
// and nothing repaints in between. At 1 s the poll itself was half the flicker.
constexpr uint32_t kFactsMs = 10000;
// Battery is an ADC read and the card's free space is a statfs: about 200 ms a
// year at this cadence, and About is the only screen that reads them.
constexpr uint32_t kSlowFactsMs = 60000;
// The Phone screen's own poll. It is a handful of atomic reads (the controller,
// the link, the window's deadline), and 250 ms is what makes a phone that
// connects land on the glass while the person is still holding it.
constexpr uint32_t kPhonePollMs = 250;
// How long a discarded capture's reason keeps Home's one line. Long enough to be
// read after a full refresh has settled, short enough that the counts line the
// person expects on Home is back before they look twice.
constexpr uint32_t kNoticeTtlMs = 5000;
// How long the sleeping face may hold the app task after its flush. It has to
// outlast a full waveform - measured at 2,122 ms on this unit - because calling
// shutdown() before the panel settles is a guaranteed half-updated frame: the
// latch is dropped by our own power_off() the moment this returns. The bound is
// here only so a panel that never answers cannot hold the power-off; 2.6 s
// covers the driver's 300 ms coalesce gap plus a full update with a little room.
// A PWR key released before the latch's own 2.5 s cutoff (docs/hardware.md)
// leaves this the only power cut, which is the case the face is drawn for.
constexpr uint32_t kSleepPanelMs = 2600;

// Why the Phone window could not open, in words, for the log. The screen has its
// own copy of these (strings.hpp); this is the serial line an operator reads.
const char* refusal_name(notesui::PhoneRefusal refusal) {
  switch (refusal) {
    case notesui::PhoneRefusal::None: return "no reason";
    case notesui::PhoneRefusal::TableFull: return "the pairing table is full";
    case notesui::PhoneRefusal::RadioBusy: return "the radio is busy";
    case notesui::PhoneRefusal::BleOff: return "Bluetooth is off";
  }
  return "?";
}

const char* effect_name(notesui::Effect effect) {
  switch (effect) {
    case notesui::Effect::None: return "no effect";
    case notesui::Effect::PowerOff: return "a power-off";
    case notesui::Effect::BeginRecording: return "the start of a recording";
    case notesui::Effect::EndRecording: return "the end of a recording";
    case notesui::Effect::DiscardRecording: return "a discarded recording";
    case notesui::Effect::StartPlayback: return "playback";
    case notesui::Effect::StopPlayback: return "a playback stop";
    case notesui::Effect::SyncNow: return "a sync";
    case notesui::Effect::OpenWifiSetup: return "the Wi-Fi setup page";
    case notesui::Effect::OpenPhoneWindow: return "the phone window";
    case notesui::Effect::ClosePhoneWindow: return "the phone window's close";
    case notesui::Effect::PairingAllow: return "a pairing answer";
    case notesui::Effect::PairingRefuse: return "a pairing refusal";
    case notesui::Effect::DeleteNote: return "a delete";
    case notesui::Effect::WipeNotes: return "a wipe of every note";
    case notesui::Effect::FormatCard: return "a card format";
    case notesui::Effect::UnpairPeer: return "a phone unpaired";
  }
  return "an effect";
}

const char* screen_name(notesui::Screen screen) {
  switch (screen) {
    case notesui::Screen::Home: return "home";
    case notesui::Screen::Recording: return "recording";
    case notesui::Screen::Menu: return "menu";
    case notesui::Screen::NoteList: return "notes";
    case notesui::Screen::NoteAction: return "note";
    case notesui::Screen::ConfirmDelete: return "confirm-delete";
    case notesui::Screen::ConfirmWipe: return "confirm-wipe";
    case notesui::Screen::ConfirmFormat: return "confirm-format";
    case notesui::Screen::Wiping: return "wiping";
    case notesui::Screen::UnpairList: return "unpair";
    case notesui::Screen::ConfirmUnpair: return "confirm-unpair";
    case notesui::Screen::Playing: return "playing";
    case notesui::Screen::Pairing: return "pairing";
    case notesui::Screen::Phone: return "phone";
    case notesui::Screen::About: return "about";
    case notesui::Screen::Back: return "back";
  }
  return "?";
}

// The Unpair screen's bond store (ui_peers.hpp), over the radio: the adapter's
// translation between the UI's plain Peer and port_ble's BondInfo, so neither
// layer has to know the other's types.
class RadioBonds : public notesui::BondPort {
 public:
  explicit RadioBonds(PhoneLink* radio) : radio_(radio) {}
  uint8_t list(notesui::Peer* out, uint8_t capacity, bool* answered) override {
    if (answered != nullptr) *answered = false;
    if (radio_ == nullptr || out == nullptr || capacity == 0) return 0;
    // The bond store lives in the radio's own host, and that host is deinitialised
    // when the window closes: opening this screen (or `ui peers`) after the radio
    // had gone to rest dereferenced freed memory and panicked the app task. The
    // window costs a few hundred ms and no pairing: the person asked to see the
    // table, so the radio it lives in has to be up.
    if (!radio_->radio_up()) radio_->wake();
    BondInfo found[notesui::kMaxPeerRows]{};
    const uint8_t cap = capacity < notesui::kMaxPeerRows ? capacity : notesui::kMaxPeerRows;
    uint8_t n = 0;
    if (!radio_->bonded_peers(found, cap, &n)) return 0;  // the store did not speak
    if (answered != nullptr) *answered = true;
    for (uint8_t i = 0; i < n; ++i) {
      out[i] = notesui::Peer{};
      out[i].type = found[i].type;
      std::memcpy(out[i].addr, found[i].addr, sizeof out[i].addr);
    }
    return n;
  }
  bool erase(const notesui::Peer& peer) override {
    if (radio_ == nullptr) return false;
    // The same window the list needed: the question can outlive the one the list
    // opened, and a hold on a resting radio must not reach into a freed host.
    if (!radio_->radio_up()) radio_->wake();
    BondInfo target{};
    target.type = peer.type;
    std::memcpy(target.addr, peer.addr, sizeof target.addr);
    return radio_->forget_peer(target);
  }

 private:
  PhoneLink* radio_;
};

// apply() can reach itself (a pairing answer hands the screen back), and the
// runtime's publish must wait for the outermost one: a flag, saved and set on
// the way in, put back on the way out.
struct InApply {
  bool& flag;
  bool was;
  explicit InApply(bool& f) : flag(f), was(f) { flag = true; }
  ~InApply() { flag = was; }
};

}  // namespace

void NotesUi::begin(const Ports& ports) {
  ports_ = ports;
  // The Unpair screen's table is the radio's: one adapter over PhoneLink, built
  // before the first frame so the state machine holds it from boot.
  bonds_ = ports_.radio != nullptr ? std::make_unique<RadioBonds>(ports_.radio) : nullptr;
  if (ports_.system != nullptr && ports_.storage != nullptr) {
    auto name = ports_.storage->get("name");
    device_name_ = name && !name->empty() ? *name : std::string(CONFIG_HG_DEFAULT_NAME);
  } else {
    device_name_ = CONFIG_HG_DEFAULT_NAME;
  }

  // The ports' first answers, before the first frame: the band's link and clock,
  // and one slow read for the battery and About's card space. Leaving them out
  // is what froze the boot frame (QA D1: clock_set defaulted true, wifi_up
  // false, and nothing ever corrected them).
  facts_ = read_facts();
  poll_facts(ports_.system != nullptr ? ports_.system->now_ms() : 0, true);

  notesui::Snapshot seed;
  std::snprintf(seed.firmware, sizeof seed.firmware, "%s",
                ports_.firmware != nullptr ? ports_.firmware : "");
  std::snprintf(seed.device, sizeof seed.device, "%s", device_name_.c_str());
  seed.note_max_s = ports_.config != nullptr ? static_cast<uint32_t>(ports_.config->note_max_s()) : 86400;
  seed.wifi_up = facts_.wifi_up;
  seed.wifi_on = facts_.wifi_on;
  seed.clock_set = facts_.clock_set;
  if (facts_.clock.valid) notesui::format_clock(facts_.clock, seed.clock_text);
  seed.bt = facts_.bt;
  seed.card_free_bytes = facts_.card_free_bytes;
  seed.battery_pct = facts_.battery_pct;
  seed.external = facts_.external;

  state_.begin(seed, ports_.store, bonds_.get());
  begun_ = true;
  first_frame_ = true;
  render(notesui::Refresh::Full);
  ESP_LOGI(TAG, "screen %s, %u notes (%u to send), card %s, clock %s, wifi %s", screen_name(state_.screen()),
           static_cast<unsigned>(state_.snapshot().note_count),
           static_cast<unsigned>(state_.snapshot().count_pending),
           state_.snapshot().card ? "up" : "none", facts_.clock_set ? "set" : "unset",
           facts_.wifi_up ? "up" : "down");
}

notesui::Facts NotesUi::read_facts() const {
  // The slow facts are carried over: only poll_facts(slow) re-reads them.
  notesui::Facts f = facts_;
  f.wifi_up = ports_.wifi != nullptr && !ports_.wifi->current().empty();
  // The radio's own answer, not the window's: a station that is up but still
  // joining is "on" and must not look like "off" (Jake, 2026-11-22).
  f.wifi_on = ports_.wifi != nullptr && ports_.wifi->radio_running();
  hg::WallTime wall;
  const bool have_time = ports_.system != nullptr && ports_.system->wall_time(wall);
  f.clock_set = have_time;
  f.clock.valid = have_time;
  if (have_time) {
    f.clock.year = wall.year;
    f.clock.month = wall.month;
    f.clock.day = wall.day;
    f.clock.hour = wall.hour;
    f.clock.minute = wall.minute;
  }
  // Bluetooth is the radio's answer, not a guess from the Phone screen: `radio`
  // prints the same three states, so the mark and the console cannot disagree.
  // The window is not only the Phone screen's - the boot window and `phone wake`
  // both advertise with the glass on Home - so reading the phone window alone
  // would leave the mark on Wi-Fi for every state the band can actually show.
  f.bt = notesui::BtState::Off;
  if (ports_.radio != nullptr) {
    const PhoneWindow window = ports_.radio->phone_window();
    if (window.connected) {
      f.bt = notesui::BtState::Connected;
    } else if (ports_.radio->radio_up() || window.left_ms != 0) {
      f.bt = notesui::BtState::Advertising;
    }
  }
  return f;
}

void NotesUi::poll_facts(uint32_t now_ms, bool slow) {
  if (slow && begun_) {
    if (ports_.power != nullptr) {
      const auto status = ports_.power->read();
      if (status) {
        // The same conversion the runtime prints: this board's port fills
        // battery_mv and never battery_percent, so copying the percent alone
        // left the band's level -1 forever (it drew '?' unplugged while
        // `notes status` said 90 %). -1 still means "no reading", never 0 %.
        facts_.battery_pct = static_cast<int8_t>(hgp::battery_percent(*status, ports_.battery_curve));
        facts_.external = status->external_power.value_or(false);
      } else {
        // A failed read is not a reading (Power::read's contract): the band says
        // '?' rather than keeping the last level as if it were live.
        facts_.battery_pct = -1;
      }
    }
    uint64_t total = 0, free_bytes = 0;
    // About's card line: the mount is the console `sd`'s source too, so the two
    // answers cannot disagree (QA D3 - it read 0.0G because nothing wrote it).
    if (sd_mount::space(&total, &free_bytes)) facts_.card_free_bytes = free_bytes;
  }
  facts_ = read_facts();
  if (!begun_) return;
  const notesui::Outcome out = state_.set_facts(facts_, now_ms);
  apply(out, now_ms);
}

// The Phone screen's window, as the port sees it: the controller, the link, the
// deadline and the pairing table. A window that is over (left_ms 0) is Closed;
// one that is up is Waiting, or Connected when a phone is on it; one whose
// controller is still coming up is Advertising.
notesui::PhoneStatus NotesUi::read_phone() const {
  notesui::PhoneStatus status;
  if (ports_.radio == nullptr) {
    status.phase = notesui::PhonePhase::Refused;
    status.refusal = notesui::PhoneRefusal::BleOff;
    return status;
  }
  const PhoneWindow window = ports_.radio->phone_window();
  status.paired = window.paired;
  status.slots = window.slots;
  status.connected = window.connected;
  status.left_ms = window.left_ms;
  status.phase = window.left_ms == 0      ? notesui::PhonePhase::Closed
                 : window.connected       ? notesui::PhonePhase::Connected
                 : window.ready           ? notesui::PhonePhase::Waiting
                                          : notesui::PhonePhase::Advertising;
  return status;
}

// Only the Phone screen asks for this. Everything else in the screens is driven
// by events; this is a countdown and a link, so it is polled - on the app task,
// on its own clock, and never from the draw path.
void NotesUi::poll_phone(uint32_t now_ms) {
  if (!begun_ || state_.screen() != notesui::Screen::Phone) {
    last_phone_ms_ = 0;
    return;
  }
  if (last_phone_ms_ != 0 && now_ms - last_phone_ms_ < kPhonePollMs) return;
  last_phone_ms_ = now_ms == 0 ? 1 : now_ms;
  apply(state_.set_phone(read_phone(), now_ms), now_ms);
}

// Why the device may not be emptied right now, as the runtime says it. Polled
// with the other facts (a key or a tick), never from the draw path, and published
// only while one of the two confirms is up: the line has no other home.
void NotesUi::poll_busy(uint32_t now_ms) {
  (void)now_ms;
  if (!begun_ || !capture_probe_.busy) return;
  const char* why = capture_probe_.busy();
  if (!state_.set_busy(why != nullptr ? why : "")) return;
  if (state_.screen() == notesui::Screen::ConfirmWipe ||
      state_.screen() == notesui::Screen::ConfirmFormat ||
      state_.screen() == notesui::Screen::ConfirmUnpair) {
    publish(notesui::DirtyQueue);
  }
}

// The wipe/format the runtime is running (t_0070a731). The counts only reach the
// glass as the job walks, and the result moves the face off WIPING to Home with
// the card read again - 0 NOTES after a wipe. Both are asked on the tick, on the
// app task; the worker owns the card and nothing here waits for it.
void NotesUi::poll_card_job(uint32_t now_ms) {
  if (!begun_) return;
  if (card_job_probe_.active) {
    bool format = false;
    uint32_t done = 0, total = 0;
    if (card_job_probe_.active(&format, &done, &total)) {
      apply(state_.set_card_job(format, done, total), now_ms);
    }
  }
  if (!card_job_probe_.result) return;
  bool ok = true;
  std::string why;
  if (!card_job_probe_.result(&ok, &why)) return;  // still running
  if (!ok) {
    ESP_LOGW(TAG, "the card job failed: %s", why.empty() ? "no reason given" : why.c_str());
  } else {
    ESP_LOGI(TAG, "the card job finished; the counts are read again");
  }
  // The runtime has already put its counts back (take_card_job_result); this
  // publish re-reads the card for the frame, which is the Home frame with the
  // number the job left. `at_least` is the machine's own class for the hand-over.
  const notesui::Outcome home = state_.card_job_finished();
  if (home.draw) publish(static_cast<uint16_t>(notesui::DirtyQueue | notesui::DirtyList), home.refresh);
}

void NotesUi::render(notesui::Refresh refresh) {
  if (asleep_) return;  // the sleeping face is the last frame: nothing paints over it
  if (ports_.display == nullptr) return;
  uint16_t* fb = ports_.display->framebuffer();
  if (fb == nullptr) return;
  const hg::DisplayInfo info = ports_.display->info();
  if (info.width == 0 || info.height == 0) return;
  hg::Canvas canvas(fb, info.width, info.height, info.swap_bytes);
  const notesui::ScreenDraw frame = state_.app().frame();
  notesui::draw(canvas, frame);

  // Entering or leaving a screen is a hand-over, and the machine decides its
  // class: a partial over the band either page inked, or a full refresh for the
  // one that spends the ghost budget (§15.2). The driver owns the rest - the
  // diff band, the window, and its own kFullEvery cadence.
  if (refresh == notesui::Refresh::Full || first_frame_) {
    ports_.display->request_full_refresh();
    first_frame_ = false;
  }
  ++frames_;
  ports_.display->flush(0, info.height);
}

// Idle: nobody has touched the device for a while, on the cell, with nothing
// running. The panel holds its picture with the rail down, so the screen costs
// nothing to keep - and every render is refused until a key brings it back, which
// also means the 10 s facts poll and the band's clock cannot spend the battery
// refreshing a page no one is reading (Jake, 2026-11-24: 6 hours idle).
void NotesUi::idle(bool on) {
  if (!begun_) return;
  if (on) {
    if (asleep_) return;
    asleep_ = true;
    if (ports_.display != nullptr) ports_.display->panel_power(false);
    return;
  }
  if (!asleep_) return;
  asleep_ = false;
  // The rail comes up here as well as in flush(): the wake is a full redraw by
  // definition (the panel lost its previous-frame RAM with the power).
  if (ports_.display != nullptr) ports_.display->panel_power(true);
  render(notesui::Refresh::Full);
}

void NotesUi::wake_from_sleep_face() {
  if (!begun_ || !asleep_) return;
  asleep_ = false;
  // A full frame: the moon has to leave the glass completely, and a partial
  // update over a full-page drawing is the one place ghosting always shows.
  render(notesui::Refresh::Full);
}

void NotesUi::sleep_face() {
  if (!begun_ || ports_.display == nullptr) return;
  asleep_ = true;  // set first: no event between here and the flush may draw
  uint16_t* fb = ports_.display->framebuffer();
  if (fb == nullptr) return;
  const hg::DisplayInfo info = ports_.display->info();
  if (info.width == 0 || info.height == 0) return;

  hg::Canvas canvas(fb, info.width, info.height, info.swap_bytes);
  notesui::draw_sleep(canvas);
  // The machine owns the class of every frame, this one included: full, so the
  // page the person was looking at does not ghost through the moon.
  const notesui::Outcome out = state_.app().sleep_face();
  if (out.refresh == notesui::Refresh::Full) ports_.display->request_full_refresh();
  ++frames_;
  // Wait for the waveform, not for the driver's task: shutdown() is called the
  // moment this returns, and on battery that drops the rail - an unfinished
  // update would leave the panel half-written, which is not "off", it is "broken".
  const uint32_t before = ports_.display->update_count();
  ports_.display->flush(0, info.height);
  const bool settled = ports_.display->wait_idle(before, kSleepPanelMs);
  ESP_LOGI(TAG, "the sleeping face is on the panel (full refresh, %s)", settled ? "settled" : "cut short");
}

void NotesUi::draw(const notesui::Outcome& outcome, uint32_t now_ms) {
  if (publish_pending_) {
    // Something the effect did moved the card (a note committed, a delete, the
    // card lost). The snapshot behind this frame is stale, so publish instead of
    // drawing it and then drawing the truth a moment later.
    publish_pending_ = false;
    const uint16_t dirty = static_cast<uint16_t>(pending_dirty_ | notesui::DirtyQueue | notesui::DirtyList);
    pending_dirty_ = notesui::DirtyNone;
    const notesui::Outcome after = state_.publish(now_ms, dirty, outcome.refresh);
    if (after.draw) render(after.refresh);
    return;
  }
  if (outcome.draw) render(outcome.refresh);
}

void NotesUi::publish(uint16_t dirty, notesui::Refresh at_least) {
  if (!begun_) return;
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  const notesui::Outcome out = state_.publish(now, dirty, at_least);
  apply(out, now);
}

void NotesUi::apply(const notesui::Outcome& outcome, uint32_t now_ms) {
  const InApply guard(in_apply_);
  // The screen may have just moved: a cursor step rotates the list's window and
  // opening a note changes the index, so the stubs this frame will read must be
  // on hand *before* it draws. UiState does that inside each entry point; the
  // draw path itself still reads no card (§15.4).
  if (!begun_) return;
  // A wipe or a format is 20-90 s of card work (measured: 114 notes in ~90 s, a
  // 64 GB format in 22.8 s), so the runtime runs it on its own task and this
  // function only starts it (t_0070a731). The busy frame goes on the panel
  // first: what the person sees while it runs is WIPING/FORMATTING, not the
  // confirm they just answered, and the app task is free for the console and the
  // keys throughout.
  if (outcome.effect == notesui::Effect::WipeNotes || outcome.effect == notesui::Effect::FormatCard) {
    if (outcome.draw) render(outcome.refresh);
    if (!hook_ || !hook_(outcome.effect, outcome.note)) {
      // No runtime took it (or it refused: no card, busy after the guard
      // passed): the busy face must not stay up over work that is not running.
      ESP_LOGW(TAG, "the runtime did not start %s", effect_name(outcome.effect));
      const notesui::Outcome undone = state_.decline(outcome.effect);
      if (undone.draw) draw(undone, now_ms);
    }
    return;
  }
  // The runtime owns the work that is not a screen: recording, the sync window
  // and the shutdown. When it takes an effect the screens do not repeat it.
  if (hook_ && outcome.effect != notesui::Effect::None && hook_(outcome.effect, outcome.note)) {
    // The runtime's answer is the truth and it can differ from the effect the
    // screens performed: a BeginRecording taken while an upload is in flight is
    // kept and queued, not started. Ask it before painting, so the first frame is
    // the queued face and not a counter over a note that is not being made.
    const notesui::Outcome truth = sync_capture();
    draw(truth.draw ? truth : outcome, now_ms);
    return;
  }
  switch (outcome.effect) {
    case notesui::Effect::None:
      break;
    case notesui::Effect::PowerOff:
      stop_playback();
      ESP_LOGI(TAG, "PWR: powering off");
      if (ports_.power != nullptr) ports_.power->power_off();
      break;
    case notesui::Effect::BeginRecording:
    case notesui::Effect::EndRecording:
    case notesui::Effect::DiscardRecording: {
      // The runtime declined (no microphone, no card, or a note already in
      // flight). This class used to record on its own here, which made an
      // unqueued note nobody counted (QA D6); it then bounced Home through a
      // synthetic key press, which asked the runtime to stop a capture that had
      // never started. Undo the move the effect was for and draw the screen the
      // device is really on: a decline must not leave RECORDING on the glass.
      ESP_LOGW(TAG, "the runtime declined %s: nothing is being captured", effect_name(outcome.effect));
      const notesui::Outcome undone = state_.decline(outcome.effect);
      if (undone.draw) {
        draw(undone, now_ms);
      } else {
        draw(outcome, now_ms);
      }
      return;
    }
    case notesui::Effect::StartPlayback:
      start_playback(outcome.note);
      break;
    case notesui::Effect::StopPlayback:
      stop_playback();
      break;
    case notesui::Effect::SyncNow:
      // run_effect() hands this to Runtime::sync_now; without a hook there is no
      // window to open.
      ESP_LOGW(TAG, "no runtime to sync with");
      break;
    case notesui::Effect::OpenPhoneWindow: {
      // The row's own screen. The port answers whether the window opened and,
      // when it did not, why - and that answer is what the glass shows instead
      // of a flash back to where the person came from (t_9972e2a1).
      notesui::PhoneStatus status;
      if (ports_.radio == nullptr) {
        status.phase = notesui::PhonePhase::Refused;
        status.refusal = notesui::PhoneRefusal::BleOff;
        ESP_LOGW(TAG, "the Phone row has no radio to open");
      } else {
        switch (ports_.radio->open_phone_window()) {
          case PhoneOpen::Opened:
            status = read_phone();
            break;
          case PhoneOpen::TableFull:
            status.phase = notesui::PhonePhase::Refused;
            status.refusal = notesui::PhoneRefusal::TableFull;
            break;
          case PhoneOpen::RadioBusy:
            status.phase = notesui::PhonePhase::Refused;
            status.refusal = notesui::PhoneRefusal::RadioBusy;
            break;
          case PhoneOpen::BleOff:
            status.phase = notesui::PhonePhase::Refused;
            status.refusal = notesui::PhoneRefusal::BleOff;
            break;
        }
        status.paired = ports_.radio->phone_window().paired;
        status.slots = ports_.radio->phone_window().slots;
        if (status.phase == notesui::PhonePhase::Refused) {
          ESP_LOGW(TAG, "the Phone window did not open: %s", refusal_name(status.refusal));
        }
      }
      last_phone_ms_ = ports_.system != nullptr ? ports_.system->now_ms() : 0;
      // One frame for the hand-over: the status if it moved the glass, the
      // row's own screen if it did not (the window opening is always a frame).
      const notesui::Outcome shown = state_.set_phone(status, now_ms);
      if (shown.draw) {
        apply(shown, now_ms);
      } else {
        render(notesui::Refresh::Full);
      }
      return;  // the Phone frame is drawn; the menu's turn is over
    }
    case notesui::Effect::ClosePhoneWindow:
      if (ports_.radio != nullptr) ports_.radio->close_phone_window();
      last_phone_ms_ = 0;
      break;  // the menu's own frame follows below
    case notesui::Effect::OpenWifiSetup:
      if (ports_.wifi != nullptr) ports_.wifi->start_setup();
      break;
    case notesui::Effect::PairingAllow:
      if (ports_.radio != nullptr) ports_.radio->answer_pairing(true);
      apply(state_.pairing_answer(true), now_ms);
      return;  // pairing_answer already drew
    case notesui::Effect::PairingRefuse:
      if (ports_.radio != nullptr) ports_.radio->answer_pairing(false);
      apply(state_.pairing_answer(false), now_ms);
      return;
    case notesui::Effect::WipeNotes:
    case notesui::Effect::FormatCard: {
      // Unreachable while a runtime hook exists (the branch above returns for
      // both), and kept for the build that has none: the screens are on the busy
      // face with nothing behind it, so go back to the truth - Home.
      ESP_LOGW(TAG, "no runtime to run %s: nothing was emptied", effect_name(outcome.effect));
      const notesui::Outcome undone = state_.decline(outcome.effect);
      if (undone.draw) draw(undone, now_ms);
      return;
    }
    case notesui::Effect::DeleteNote: {
      const std::string* found_id = state_.cache().id_at(outcome.note);
      if (ports_.store == nullptr || found_id == nullptr) break;
      const std::string id = *found_id;
      notes::NoteState found = notes::NoteState::Pending;
      if (ports_.store->erase(id, &found)) {
        ESP_LOGI(TAG, "deleted %s", id.c_str());
        // The frame about to be drawn is the list without this row: the counts
        // and the window are re-read on this event, not in the draw path.
        publish_pending_ = true;
        pending_dirty_ = static_cast<uint16_t>(pending_dirty_ | notesui::DirtyQueue | notesui::DirtyList);
      } else {
        ESP_LOGW(TAG, "could not delete %s", id.c_str());
      }
      break;
    }
    case notesui::Effect::UnpairPeer: {
      // The screens own this one: the bond store is the radio's, and the UiState
      // adapter does the erase (exactly one) and republishes. The frame drawn is
      // the list one row shorter; the runtime never sees the effect.
      if (bonds_ == nullptr) {
        ESP_LOGW(TAG, "no radio in this build: nothing to unpair");
        break;
      }
      const notesui::Outcome after = state_.unpair(outcome.peer, now_ms);
      if (after.draw) {
        draw(after, now_ms);
        return;
      }
      break;
    }
  }
  draw(outcome, now_ms);
}

const char* NotesUi::screen_word() const { return screen_name(state_.screen()); }

notesui::Outcome NotesUi::press(notesui::Key key) {
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  poll_facts(now, false);  // the band is a fact, not a tick
  poll_busy(now);          // and so is the reason a wipe or a format is refused
  const notesui::Outcome out = state_.press(key, now);
  apply(out, now);
  ESP_LOGI(TAG, "key %u -> screen %s", static_cast<unsigned>(key), screen_name(state_.screen()));
  return out;
}

void NotesUi::tick(uint32_t now_ms) {
  if (ports_.display != nullptr && ports_.display->take_redraw_request()) {
    // A transfer was lost; the driver has already forced a full waveform, so
    // all the app has to do is draw the same screen again (§15.2).
    render(notesui::Refresh::Full);
  }

  // The capture first: the runtime's answer decides which face the rest of this
  // pass draws, and a capture that ended under the glass is worth the frame.
  const notesui::Outcome followed = sync_capture();
  if (followed.draw) apply(followed, now_ms);

  // A wipe or a format is on the runtime's own task: take its counts and, when
  // it is over, put Home back with the card re-read (t_0070a731).
  poll_card_job(now_ms);

  const uint32_t since_facts = now_ms - last_facts_ms_;
  if (last_facts_ms_ == 0 || since_facts >= kFactsMs) {
    const bool slow = last_slow_facts_ms_ == 0 || now_ms - last_slow_facts_ms_ >= kSlowFactsMs;
    last_facts_ms_ = now_ms == 0 ? 1 : now_ms;
    if (slow) last_slow_facts_ms_ = last_facts_ms_;
    poll_facts(now_ms, slow);
  }

  // The Phone screen: a countdown and a link, the only things on the glass that
  // move without an event.
  poll_phone(now_ms);

  // A capture or an upload that starts or ends while a confirm is open changes
  // whether its hold would be refused: the line is a fact like the band's.
  poll_busy(now_ms);

  // Playback publishes its position; the screen draws at 5 s (§9.2).
  if (play_state_.load() == Play::Running) {
    const uint32_t pos = play_pos_s_.load();
    if (pos != state_.snapshot().play_pos_s) {
      state_.set_play(pos, 0);
      apply(state_.tick(now_ms), now_ms);
    }
    if (play_done_.exchange(false)) {
      apply(state_.playback_finished(), now_ms);
      stop_playback();
    }
  } else {
    apply(state_.tick(now_ms), now_ms);
  }
}

notesui::Outcome NotesUi::sync_capture() {
  if (!capture_probe_.recording) return notesui::Outcome{};  // no probe wired
  // The value is published every pass, not only while it is true: a capture the
  // console ended has to reach the glass too (the screen follows the runtime).
  const bool recording = capture_probe_.recording();
  // A queued tap is not a capture: `recording()` is false until the upload ends
  // and the runtime starts the note. The glass still has to draw the wait.
  const bool queued = capture_probe_.queued ? capture_probe_.queued() : false;
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  const uint32_t elapsed = capture_probe_.elapsed_s ? capture_probe_.elapsed_s() : 0;
  // The cap the capture started with, not the setting's default: a note is
  // limited by the card's room as often as by `note_max_s`.
  const uint32_t cap_s = capture_probe_.cap_s && recording ? capture_probe_.cap_s() : 0;
  const uint64_t cap_bytes = capture_probe_.cap_bytes && recording ? capture_probe_.cap_bytes() : 0;
  static bool was_queued = false;
  if (was_queued && recording) ESP_LOGI(TAG, "a queued tap started: the probe says %u s", static_cast<unsigned>(elapsed));
  was_queued = queued;
  return state_.set_capture(recording, elapsed, queued, now, cap_s, cap_bytes);
}

void NotesUi::start_playback(uint16_t index) {
  const std::string* found = state_.cache().id_at(index);
  if (ports_.speaker == nullptr || ports_.store == nullptr || found == nullptr) return;
  if (play_task_ != nullptr) return;  // one at a time
  const std::string& id = *found;
  notes::NoteState state = notes::NoteState::Pending;
  if (!ports_.store->locate(id, state)) return;
  notes::WavInfo info;
  if (!reader_.open(ports_.store->audio_path(state, id), info)) {
    ESP_LOGW(TAG, "cannot open %s", id.c_str());
    return;
  }
  if (play_buffer_ == nullptr) {
    // Internal RAM: the I2S DMA reads this buffer, and PSRAM is not what the
    // channel's descriptors were set up for. 100 ms of mono PCM16 is 4.8 KB.
    play_buffer_ = static_cast<int16_t*>(heap_caps_malloc(kPlayFrames * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    if (play_buffer_ == nullptr) {
      reader_.close();
      return;
    }
  }
  if (!ports_.speaker->begin(info.sample_rate == 0 ? notes::kSampleRate : info.sample_rate)) {
    reader_.close();
    return;
  }
  play_pos_s_.store(0);
  play_stop_.store(false);
  play_done_.store(false);
  play_state_ = Play::Running;
  // The note's own length, not the stub's: a truncated WAV would otherwise
  // promise more than it has.
  state_.set_play(0, info.duration_s);
  if (xTaskCreate(&NotesUi::play_task, "note-play", 4096, this, 4, &play_task_) != pdPASS) {
    play_task_ = nullptr;
    play_state_ = Play::Idle;
    ports_.speaker->abort();
    reader_.close();
  }
}

void NotesUi::play_task(void* arg) { static_cast<NotesUi*>(arg)->play_loop(); }

void NotesUi::play_loop() {
  for (;;) {
    if (play_stop_.load()) break;
    const size_t frames = reader_.read(play_buffer_, kPlayFrames);
    if (frames == 0) {
      play_done_.store(true);  // end of the audio: the screen goes back on its own
      break;
    }
    ports_.speaker->write(play_buffer_, frames);
    play_pos_s_.store(reader_.position_s());
    // Keep one buffer of headroom: write only while the port is not still busy
    // playing the previous chunk.
    while (ports_.speaker->busy() && !play_stop_.load()) vTaskDelay(pdMS_TO_TICKS(kPlayPaceMs));
  }
  if (play_stop_.load()) ports_.speaker->abort();
  else ports_.speaker->end();
  reader_.close();
  play_state_ = Play::Idle;
  play_task_ = nullptr;
  vTaskDelete(nullptr);
}

void NotesUi::stop_playback() {
  if (play_state_.load() == Play::Idle && play_task_ == nullptr) return;
  play_stop_.store(true);
  // The task aborts the codec and closes the file within one 20 ms wait; the
  // app task does not wait for it, so a stop never stalls a key press.
  play_state_ = Play::Stopping;
}

void NotesUi::on_pairing_ask(const uint8_t peer[6]) {
  apply(state_.pairing_ask(0, peer), ports_.system ? ports_.system->now_ms() : 0);
}

void NotesUi::on_pairing_passkey(uint32_t passkey) {
  // The passkey replaces the dashes on the ask; nothing else about the screen
  // moves, so this is a partial redraw.
  const uint32_t now = ports_.system ? ports_.system->now_ms() : 0;
  apply(state_.pairing_ask(passkey), now);
}

void NotesUi::on_pairing_paired(const uint8_t peer[6]) {
  // The bond is made: the question on the glass is over, and the screens say so
  // by themselves - the person may have answered it on the console (`phone
  // allow`), which never reaches this screen, and a phone that is counted as
  // paired must not leave PAIRING over it (t_da500270).
  apply(state_.pairing_paired(peer), ports_.system ? ports_.system->now_ms() : 0);
}

void NotesUi::on_runtime_change() {
  if (!begun_) return;
  if (in_apply_) {
    // An effect is mid-flight: the runtime moved the card while the screen is
    // still deciding its frame. draw() publishes it once the effect returns.
    publish_pending_ = true;
    pending_dirty_ = static_cast<uint16_t>(pending_dirty_ | notesui::DirtyQueue | notesui::DirtyList);
    return;
  }
  // A capture that started or ended is the runtime's news too: take its own
  // answer before drawing, so a stop made from the console cannot leave the
  // counter running over a note already on the card.
  const notesui::Outcome followed = sync_capture();
  publish(static_cast<uint16_t>(notesui::DirtyQueue | notesui::DirtyList | notesui::DirtyRecording),
          followed.draw ? notesui::Refresh::Full : notesui::Refresh::None);
}

void NotesUi::on_upload(bool active, uint8_t done, uint8_t total) {
  if (!begun_) return;
  state_.set_upload(active, done, total);
  publish(static_cast<uint16_t>(notesui::DirtyUpload | notesui::DirtyQueue));
}

void NotesUi::on_upload_failed() {
  if (!begun_) return;
  state_.set_upload_failed();
  publish(notesui::DirtyUpload);
}

void NotesUi::on_discarded() {
  if (!begun_) return;
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  // The words are the screens': the runtime says what happened (no audio), the
  // glass says it in the slot it has. Adopted here, drawn by the frame the stop
  // already owes - never a second full refresh for one line.
  state_.set_notice(notesui::sub::kNoAudioSaved, now, kNoticeTtlMs);
  if (in_apply_) {
    // The key that stopped the capture is mid-effect: its own frame is coming,
    // and this line belongs on it (on_runtime_change's rule).
    publish_pending_ = true;
    pending_dirty_ = static_cast<uint16_t>(pending_dirty_ | notesui::DirtyNotice);
    return;
  }
  // Ended from the console or the runtime: on Home only, because that is the
  // screen with the slot. Anywhere else the notice rides the publish
  // on_runtime_change is about to make when the capture ends, or the next Home
  // frame if it has lapsed by then.
  if (state_.screen() == notesui::Screen::Home) publish(notesui::DirtyNotice);
}

std::string NotesUi::console(std::string_view line) {
  if (line == "ui") {
    const notesui::Snapshot& snap = state_.snapshot();
    // `ble` is the band's own mark and `frames` is every panel refresh since
    // boot, so both §4's mark and §5's flicker budget are measurable over the
    // console: sample `ui` twice and subtract.
    const char* bt = snap.bt == notesui::BtState::Connected     ? "connected"
                     : snap.bt == notesui::BtState::Advertising ? "advertising"
                                                                : "off";
    char out[240];
    std::snprintf(out, sizeof out,
                  "screen %s, %u notes, %u to send, %u failed, card %s, wifi %s, clock %s, ble %s, frames %u",
                  screen_name(state_.screen()), static_cast<unsigned>(snap.note_count),
                  static_cast<unsigned>(snap.count_pending), static_cast<unsigned>(snap.count_failed),
                  snap.card ? "up" : "none", snap.wifi_up ? "up" : "down", snap.clock_set ? "set" : "unset",
                  bt, static_cast<unsigned>(frames_));
    return out;
  }
  if (line == "ui home") {
    // The bench's clean-page verb: one full refresh of whatever is on the glass,
    // which is the same waveform `epd full` arms on the driver. A hand-over no
    // longer draws this way (§15.2) - this is the deliberate clean one.
    render(notesui::Refresh::Full);
    return std::string("@ok redrawn");
  }
  if (line == "ui peers") {
    // The bond table as the screen lists it, for the bench: the same read the
    // Unpair screen makes, so "the list the glass drew" is checkable from the
    // console without decoding a frame.
    if (bonds_ == nullptr) return std::string("@error no radio in this build");
    notesui::Peer peers[notesui::kMaxPeerRows];
    const uint8_t n = bonds_->list(peers, notesui::kMaxPeerRows);
    std::string out = "@ok " + std::to_string(n) + " paired";
    for (uint8_t i = 0; i < n; ++i) {
      char addr[18];
      notesui::format_peer_addr(peers[i], addr);
      out += std::string(i == 0 ? ": " : ", ") + addr;
    }
    return out;
  }
  if (line.rfind("ui press ", 0) == 0) {
    // A key, for a host with no finger on the board. The same entry the GPIO
    // poll uses, so what this proves is what a press does (QA and the bench: a
    // transition that needs a physical key is a transition CI cannot reach).
    const std::string_view which(line.substr(9));
    notesui::Key key;
    if (which == "boot") key = notesui::Key::BootTap;
    else if (which == "hold") key = notesui::Key::BootHold;
    else if (which == "pwr") key = notesui::Key::Power;
    else return std::string("@error usage: ui press boot|hold|pwr");
    press(key);
    return std::string("@ok screen ") + screen_name(state_.screen());
  }
  return std::string();
}

}  // namespace hgp
