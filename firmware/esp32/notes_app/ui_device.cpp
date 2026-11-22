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
#include "notes_app/ui_render.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.ui";

// Playback reads 100 ms at a time: small enough that a stop lands within a
// frame, large enough that the card is not asked for 77 tiny reads a second.
constexpr size_t kPlayFrames = 2400;
// While a note plays, the reader keeps one buffer in the speaker and waits.
constexpr uint32_t kPlayPaceMs = 20;
// The band's facts are a string compare and a clock read: once a second is
// cheap and makes a link that came up between events land on the glass.
constexpr uint32_t kFactsMs = 1000;
// Battery is an ADC read and the card's free space is a statfs: about 200 ms a
// year at this cadence, and About is the only screen that reads them.
constexpr uint32_t kSlowFactsMs = 60000;

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
    case notesui::Effect::PairingAllow: return "a pairing answer";
    case notesui::Effect::PairingRefuse: return "a pairing refusal";
    case notesui::Effect::DeleteNote: return "a delete";
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
    case notesui::Screen::Playing: return "playing";
    case notesui::Screen::Pairing: return "pairing";
    case notesui::Screen::About: return "about";
    case notesui::Screen::Back: return "back";
  }
  return "?";
}

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
  seed.clock_set = facts_.clock_set;
  seed.card_free_bytes = facts_.card_free_bytes;
  seed.battery_pct = facts_.battery_pct;
  seed.external = facts_.external;

  state_.begin(seed, ports_.store);
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
  hg::WallTime wall;
  f.clock_set = ports_.system != nullptr && ports_.system->wall_time(wall);
  return f;
}

void NotesUi::poll_facts(uint32_t now_ms, bool slow) {
  if (slow && begun_) {
    if (ports_.power != nullptr) {
      const auto status = ports_.power->read();
      if (status) {
        facts_.battery_pct = status->battery_percent ? static_cast<int8_t>(*status->battery_percent) : -1;
        facts_.external = status->external_power.value_or(false);
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

void NotesUi::render(notesui::Refresh refresh) {
  if (ports_.display == nullptr) return;
  uint16_t* fb = ports_.display->framebuffer();
  if (fb == nullptr) return;
  const hg::DisplayInfo info = ports_.display->info();
  if (info.width == 0 || info.height == 0) return;
  hg::Canvas canvas(fb, info.width, info.height, info.swap_bytes);
  const notesui::ScreenDraw frame = state_.app().frame();
  notesui::draw(canvas, frame);

  // Entering or leaving a screen is always full; within a screen the driver's
  // partial waveform is the default (§15.2). A partial is a whole-panel
  // waveform anyway, so the flush band is about the conversion cost and not
  // about what the panel transmits.
  if (refresh == notesui::Refresh::Full || first_frame_) {
    ports_.display->request_full_refresh();
    first_frame_ = false;
  }
  ports_.display->flush(0, info.height);
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
  // The runtime owns the work that is not a screen: recording, the sync window
  // and the shutdown. When it takes an effect the screens do not repeat it.
  if (hook_ && outcome.effect != notesui::Effect::None && hook_(outcome.effect, outcome.note)) {
    draw(outcome, now_ms);
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
    case notesui::Effect::OpenPhoneWindow:
      if (ports_.radio != nullptr) ports_.radio->open_pairing();
      break;
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
  }
  draw(outcome, now_ms);
}

const char* NotesUi::screen_word() const { return screen_name(state_.screen()); }

void NotesUi::press(notesui::Key key) {
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  poll_facts(now, false);  // the band is a fact, not a tick
  const notesui::Outcome out = state_.press(key, now);
  apply(out, now);
  ESP_LOGI(TAG, "key %u -> screen %s", static_cast<unsigned>(key), screen_name(state_.screen()));
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

  const uint32_t since_facts = now_ms - last_facts_ms_;
  if (last_facts_ms_ == 0 || since_facts >= kFactsMs) {
    const bool slow = last_slow_facts_ms_ == 0 || now_ms - last_slow_facts_ms_ >= kSlowFactsMs;
    last_facts_ms_ = now_ms == 0 ? 1 : now_ms;
    if (slow) last_slow_facts_ms_ = last_facts_ms_;
    poll_facts(now_ms, slow);
  }

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
  if (!capture_probe_.recording) return notesui::Outcome{};
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  const uint32_t elapsed = capture_probe_.elapsed_s ? capture_probe_.elapsed_s() : 0;
  return state_.set_capture(capture_probe_.recording(), elapsed, now);
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

void NotesUi::on_pairing_ask() { apply(state_.pairing_ask(0), ports_.system ? ports_.system->now_ms() : 0); }

void NotesUi::on_pairing_passkey(uint32_t passkey) {
  // The passkey replaces the dashes on the ask; nothing else about the screen
  // moves, so this is a partial redraw.
  const uint32_t now = ports_.system ? ports_.system->now_ms() : 0;
  apply(state_.pairing_ask(passkey), now);
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

std::string NotesUi::console(std::string_view line) {
  if (line == "ui") {
    const notesui::Snapshot& snap = state_.snapshot();
    char out[200];
    std::snprintf(out, sizeof out, "screen %s, %u notes, %u to send, %u failed, card %s, wifi %s, clock %s",
                  screen_name(state_.screen()), static_cast<unsigned>(snap.note_count),
                  static_cast<unsigned>(snap.count_pending), static_cast<unsigned>(snap.count_failed),
                  snap.card ? "up" : "none", snap.wifi_up ? "up" : "down", snap.clock_set ? "set" : "unset");
    return out;
  }
  if (line == "ui home") {
    // A full refresh across the next frame: what a screen hand-over does.
    render(notesui::Refresh::Full);
    return std::string("@ok redrawn");
  }
  return std::string();
}

}  // namespace hgp
