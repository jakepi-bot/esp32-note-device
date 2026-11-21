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

// The panel is 200x200 at 1 bit; the counters tick at 5 s and the recording cap
// is config's (300 s by default).
constexpr uint32_t kCounterStepS = 5;
// Playback reads 100 ms at a time: small enough that a stop lands within a
// frame, large enough that the card is not asked for 77 tiny reads a second.
constexpr size_t kPlayFrames = 2400;
// While a note plays, the reader keeps one buffer in the speaker and waits.
constexpr uint32_t kPlayPaceMs = 20;
// Battery is an ADC read; the band's icon and About need it, and a note's
// metadata wants millivolts. Once a minute is enough for both.
constexpr uint32_t kPowerPollMs = 60000;

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

}  // namespace

std::string NotesUi::device_id() {
  // The same identity the Hermes build has: the stored device key, hashed. The
  // key is generated once and never leaves the device; the notes image reuses
  // it so a reflash does not orphan the notes already in the vault.
  std::string stored;
  if (ports_.storage != nullptr) {
    auto value = ports_.storage->get("device_key");
    if (value) stored = *value;
  }
  std::vector<uint8_t> key;
  if (!stored.empty()) hg::crypto::base64_decode(stored, key);
  if (key.size() != 32) {
    key.assign(32, 0);
    ports_.system->random_bytes(key.data(), key.size());
    if (ports_.storage != nullptr) {
      if (!ports_.storage->set("device_key", hg::crypto::base64_encode(key.data(), key.size()))) {
        ESP_LOGW(TAG, "could not save the device key: this identity lasts until the next restart");
      }
    }
  }
  return hg::proto::device_id_for_key(key.data(), key.size());
}

void NotesUi::begin(const Ports& ports) {
  ports_ = ports;
  if (ports_.system != nullptr) {
    device_id_ = device_id();
    auto name = ports_.storage != nullptr ? ports_.storage->get("name") : std::nullopt;
    device_name_ = name && !name->empty() ? *name : std::string(CONFIG_HG_DEFAULT_NAME);
    auto seq = ports_.storage != nullptr ? ports_.storage->get("note_seq") : std::nullopt;
    seq_ = seq ? static_cast<uint32_t>(std::strtoul(seq->c_str(), nullptr, 10)) : 0;
  }
  std::snprintf(snap_.firmware, sizeof snap_.firmware, "%s",
                ports_.firmware != nullptr ? ports_.firmware : "");
  std::snprintf(snap_.device, sizeof snap_.device, "%s", device_name_.c_str());
  snap_.note_max_s = ports_.config != nullptr ? static_cast<uint32_t>(ports_.config->note_max_s()) : 300;

  // The card was mounted by app_main; ensure() is the check that the three
  // folders exist, and its answer is Home's NO CARD.
  card_ready_ = ports_.store != nullptr && ports_.store->ensure();
  snap_.card = card_ready_;
  load_counts();
  load_window();
  refresh_power();

  app_.begin(snap_);
  first_frame_ = true;
  render(notesui::Refresh::Full);
  ESP_LOGI(TAG, "screen %s, %u notes (%u to send)", screen_name(app_.screen()), snap_.note_count,
           snap_.count_pending);
}

void NotesUi::refresh_power() {
  if (ports_.power == nullptr) return;
  const auto status = ports_.power->read();
  if (!status) return;
  snap_.battery_pct = status->battery_percent ? static_cast<int8_t>(*status->battery_percent) : -1;
  snap_.external = status->external_power.value_or(false);
  battery_mv_ = status->battery_mv ? static_cast<int>(*status->battery_mv) : 0;
  // The low-power rule (§4.2): 10 % or less, and not on a cable.
  snap_.low_power = snap_.battery_pct >= 0 && snap_.battery_pct <= 10 && !snap_.external;
}

void NotesUi::load_counts() {
  if (ports_.store == nullptr) {
    snap_.card = false;
    snap_.count_pending = snap_.count_sent = snap_.count_failed = 0;
    snap_.note_count = 0;
    snap_.stub_first = snap_.stub_rows = 0;
    return;
  }
  card_ready_ = cache_.reload(*ports_.store);
  snap_.card = card_ready_;
  snap_.count_pending = cache_.pending();
  snap_.count_sent = cache_.sent();
  snap_.count_failed = cache_.failed();
  snap_.note_count = static_cast<uint16_t>(cache_.note_count());
  snap_.stub_first = snap_.stub_rows = 0;  // the window is stale by construction
  if (ports_.radio != nullptr) ports_.radio->set_pending(snap_.count_pending);
}

void NotesUi::load_window() {
  // The app owns this answer: it is the layer that decides which stubs a frame
  // reads, so the caller cannot fill a window the renderer does not use.
  const notesui::Window want = app_.window();
  const uint8_t first = want.first;
  const uint8_t rows = want.rows;
  // Only a window that moved costs card reads (§15.4).
  if (first == snap_.stub_first && rows == snap_.stub_rows) return;
  if (ports_.store == nullptr || !card_ready_) {
    snap_.stub_first = snap_.stub_rows = 0;
    return;
  }
  cache_.fill(*ports_.store, snap_, first, rows);
}

void NotesUi::publish(uint16_t dirty) {
  // Link and clock are cheap and always read here; they are what the band shows.
  snap_.wifi_up = ports_.wifi != nullptr && !ports_.wifi->current().empty();
  hg::WallTime wall;
  snap_.clock_set = ports_.system != nullptr && ports_.system->wall_time(wall);
  snap_.upload_active = upload_active_;
  snap_.upload_failed = uploaded_failed_;
  snap_.upload_done = upload_done_;
  snap_.upload_total = upload_total_;
  snap_.play_pos_s = play_pos_s_.load();
  load_counts();
  load_window();
  const notesui::Outcome out = app_.publish(snap_, dirty);
  apply(out, ports_.system != nullptr ? ports_.system->now_ms() : 0);
}

void NotesUi::apply(const notesui::Outcome& outcome, uint32_t now_ms) {
  // The screen may have just moved: a cursor step rotates the list's window and
  // opening a note changes the index, so the stubs this frame will read must be
  // on hand *before* it draws. This is an event path, not the draw path — the
  // renderer itself still reads no card (§15.4), and the window only costs a
  // read when it actually moved.
  load_window();
  // The runtime owns the work that is not a screen: recording, the sync window
  // and the shutdown. When it takes an effect the screens do not repeat it.
  if (hook_ && outcome.effect != notesui::Effect::None && hook_(outcome.effect, outcome.note)) {
    if (outcome.draw) render(outcome.refresh);
    return;
  }
  switch (outcome.effect) {
    case notesui::Effect::None:
      break;
    case notesui::Effect::PowerOff:
      stop_playback();
      if (capture_.active()) finish_recording(false);
      ESP_LOGI(TAG, "PWR: powering off");
      if (ports_.power != nullptr) ports_.power->power_off();
      break;
    case notesui::Effect::BeginRecording:
      start_recording(now_ms);
      break;
    case notesui::Effect::EndRecording:
      finish_recording(true);
      break;
    case notesui::Effect::DiscardRecording:
      finish_recording(false);
      break;
    case notesui::Effect::StartPlayback:
      start_playback(outcome.note);
      break;
    case notesui::Effect::StopPlayback:
      stop_playback();
      break;
    case notesui::Effect::SyncNow:
      sync_requested_ = true;
      break;
    case notesui::Effect::OpenPhoneWindow:
      if (ports_.radio != nullptr) ports_.radio->open_pairing();
      break;
    case notesui::Effect::OpenWifiSetup:
      if (ports_.wifi != nullptr) ports_.wifi->start_setup();
      break;
    case notesui::Effect::PairingAllow:
      if (ports_.radio != nullptr) ports_.radio->answer_pairing(true);
      apply(app_.pairing_answer(true), now_ms);
      return;  // pairing_answer already drew
    case notesui::Effect::PairingRefuse:
      if (ports_.radio != nullptr) ports_.radio->answer_pairing(false);
      apply(app_.pairing_answer(false), now_ms);
      return;
    case notesui::Effect::DeleteNote: {
      const std::string* found_id = cache_.id_at(outcome.note);
      if (ports_.store == nullptr || found_id == nullptr) break;
      const std::string id = *found_id;
      notes::NoteState found = notes::NoteState::Pending;
      if (ports_.store->erase(id, &found)) {
        ESP_LOGI(TAG, "deleted %s", id.c_str());
        // The frame about to be drawn is the list without this row: the counts
        // and the window are re-read here, on this event, not in the draw path.
        load_counts();
        load_window();
      } else {
        ESP_LOGW(TAG, "could not delete %s", id.c_str());
      }
      break;
    }
  }
  if (outcome.draw) render(outcome.refresh);
}

void NotesUi::render(notesui::Refresh refresh) {
  if (ports_.display == nullptr) return;
  uint16_t* fb = ports_.display->framebuffer();
  if (fb == nullptr) return;
  const hg::DisplayInfo info = ports_.display->info();
  if (info.width == 0 || info.height == 0) return;
  hg::Canvas canvas(fb, info.width, info.height, info.swap_bytes);
  const notesui::ScreenDraw frame = app_.frame();
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

void NotesUi::press(notesui::Key key) {
  const uint32_t now = ports_.system != nullptr ? ports_.system->now_ms() : esp_timer_get_time() / 1000;
  refresh_power();  // one ADC read per press: the band is a fact, not a tick
  const notesui::Outcome out = app_.press(key, now);
  apply(out, now);
  ESP_LOGI(TAG, "key %u -> screen %s", static_cast<unsigned>(key), screen_name(app_.screen()));
}

void NotesUi::tick(uint32_t now_ms) {
  if (ports_.display != nullptr && ports_.display->take_redraw_request()) {
    // A transfer was lost; the driver has already forced a full waveform, so
    // all the app has to do is draw the same screen again (§15.2).
    render(notesui::Refresh::Full);
  }
  static uint32_t last_power_ms = 0;
  if (now_ms - last_power_ms >= kPowerPollMs) {
    last_power_ms = now_ms;
    refresh_power();
  }

  // Playback publishes its position; the screen draws at 5 s (§9.2).
  if (play_state_.load() == Play::Running) {
    const uint32_t pos = play_pos_s_.load();
    if (pos != snap_.play_pos_s) {
      snap_.play_pos_s = pos;
      const notesui::Outcome out = app_.tick(now_ms);
      apply(out, now_ms);
    }
    if (play_done_.exchange(false)) {
      apply(app_.playback_finished(), now_ms);
      stop_playback();
    }
  } else {
    const notesui::Outcome out = app_.tick(now_ms);
    apply(out, now_ms);
  }
}

void NotesUi::start_recording(uint32_t now_ms) {
  if (ports_.store == nullptr || ports_.mic == nullptr || ports_.system == nullptr) {
    ESP_LOGW(TAG, "no card or no microphone: nothing to record");
    return;
  }
  std::string error;
  bool started = false;
  for (int tries = 0; tries < 5 && !started; ++tries) {
    ++seq_;
    note_id_ = notes::make_note_id(device_id_, std::time(nullptr), seq_);
    started = capture_.start(*ports_.store, note_id_, snap_.note_max_s, error);
  }
  if (!started) {
    card_ready_ = false;
    sd_mount::release();
    ESP_LOGW(TAG, "cannot record: %s", error.c_str());
    return;
  }
  if (!writer_ready_) {
    writer_ready_ = writer_.begin();
    writer_.bind(&capture_);
  }
  if (ports_.storage != nullptr) ports_.storage->set("note_seq", std::to_string(seq_));
  if (!ports_.mic->start(notes::kSampleRate)) {
    ESP_LOGW(TAG, "the microphone did not start");
    finish_recording(false);
    return;
  }
  ESP_LOGI(TAG, "recording %s (cap %u s)", note_id_.c_str(), static_cast<unsigned>(snap_.note_max_s));
}

void NotesUi::on_mic_samples(const int16_t* samples, size_t count) {
  if (!capture_.active() || samples == nullptr) return;
  capture_.feed(samples, count);
  if (writer_ready_) writer_.push(samples, count);
}

void NotesUi::finish_recording(bool keep) {
  if (ports_.mic != nullptr) ports_.mic->stop();
  if (writer_ready_) writer_.drain(30000);
  if (!capture_.active()) return;
  if (!keep) {
    writer_.discard();
    capture_.abort();
    ESP_LOGI(TAG, "note discarded");
    return;
  }
  notes::Meta meta;
  meta.note_id = note_id_;
  meta.device_id = device_id_;
  meta.device_name = device_name_;
  meta.firmware = ports_.firmware != nullptr ? ports_.firmware : "";
  meta.created_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  meta.recorded_at = notes::iso8601_utc(std::time(nullptr));
  meta.battery_mv = battery_mv_;

  std::string error;
  if (!capture_.finish(meta, error)) {
    card_ready_ = false;
    sd_mount::release();
    ESP_LOGW(TAG, "note not saved: %s", error.c_str());
    return;
  }
  meta.idempotency_key = meta.note_id;
  if (!ports_.store->write_meta(meta.note_id, meta, error)) {
    ESP_LOGW(TAG, "note.json not written: %s", error.c_str());
    return;
  }
  ESP_LOGI(TAG, "note %s kept: %u s, %u bytes", meta.note_id.c_str(), meta.duration_s,
           static_cast<unsigned>(meta.bytes));
  if (ports_.radio != nullptr) ports_.radio->note_saved();
  load_counts();
  publish(notesui::DirtyQueue | notesui::DirtyList);
}

void NotesUi::start_playback(uint16_t index) {
  const std::string* found = cache_.id_at(index);
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
  snap_.play_total_s = info.duration_s;
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

void NotesUi::on_pairing_ask() { apply(app_.pairing_ask(0), ports_.system ? ports_.system->now_ms() : 0); }

void NotesUi::on_pairing_passkey(uint32_t passkey) {
  // The passkey replaces the dashes on the ask; nothing else about the screen
  // moves, so this is a partial redraw.
  const notesui::Outcome out = app_.pairing_ask(passkey);
  apply(out, ports_.system ? ports_.system->now_ms() : 0);
}

void NotesUi::on_upload(bool active, uint8_t done, uint8_t total) {
  upload_active_ = active;
  upload_done_ = done;
  upload_total_ = total;
  if (active) uploaded_failed_ = false;
  publish(notesui::DirtyUpload | notesui::DirtyQueue);
}

void NotesUi::on_upload_failed() {
  upload_active_ = false;
  uploaded_failed_ = true;
  publish(notesui::DirtyUpload);
}

bool NotesUi::take_sync_request() {
  const bool asked = sync_requested_;
  sync_requested_ = false;
  return asked;
}

std::string NotesUi::console(std::string_view line) {
  if (line == "ui") {
    char out[160];
    std::snprintf(out, sizeof out, "screen %s, %u notes, %u to send, %u failed, card %s, wifi %s",
                  screen_name(app_.screen()), static_cast<unsigned>(snap_.note_count),
                  static_cast<unsigned>(snap_.count_pending), static_cast<unsigned>(snap_.count_failed),
                  snap_.card ? "up" : "none", snap_.wifi_up ? "up" : "down");
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
