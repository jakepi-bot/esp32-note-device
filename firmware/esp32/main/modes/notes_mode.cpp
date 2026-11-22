// See notes_mode.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/notes_mode.hpp"

#include <cstdio>
#include <ctime>

#include "device_config.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "hg/app.hpp"
#include "notes/sd_mount.hpp"
#include "ui_state.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.notes";
// How often the card is scanned for the counts, and how often a mount is
// retried while no card answers (a tap asks straight away).
constexpr uint32_t kScanMs = 5000;
// With nothing queued there is nothing to notice on the card: only this device
// writes notes, and every write recounts at once (finish_note, the phone's
// transfer, the console). A scan is a directory walk plus the FAT free-cluster
// walk, so a resting card is read every 30 s instead of every 5.
constexpr uint32_t kIdleScanMs = 30000;
constexpr uint32_t kSdRetryMs = 30000;
// The retention rule's own clocks: a pass every five minutes, and every minute
// while the card is under its free-space threshold (a sync can turn a pending
// note into a deletable one, and the error has to clear as soon as it can).
constexpr uint32_t kRetentionMs = 300000;
constexpr uint32_t kRetentionLowMs = 60000;
// What the e-paper shows when the card is under the threshold and `sent/` has
// nothing left: the state, and why the device will not record into it.
constexpr const char* kLowSpaceError = "SD almost full, no synced notes to delete";
// The mount point every user of the card shares (notes/sd_mount.hpp).
constexpr const char* kSdRoot = "/sdcard";
constexpr uint32_t kMinPlausibleUnix = 1577836800;  // 2020-01-01: earlier means no clock yet
constexpr const char* kSeqKey = "note_seq";
constexpr const char* kNotesUrlKey = "notes_url";
constexpr uint16_t kDefaultNotesPort = 8766;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

std::string trim(std::string_view text) {
  size_t b = 0, e = text.size();
  while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
  while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
  return std::string(text.substr(b, e - b));
}

// The card as the Bluetooth transfer reads it: pending/ notes only, sequential
// reads, and a note moves to sent/ only when the phone says Hermes took it
// (notes_ble.hpp). The mode owns the Store and the card check, so this is the
// adapter between the two and holds no state of its own beyond the open file.
class CardBleSource final : public notes::BleSource {
 public:
  CardBleSource(notes::Store& store, std::function<bool()> card_ready)
      : store_(store), card_ready_(std::move(card_ready)) {}
  ~CardBleSource() override { close(); }

  bool ready() const override { return card_ready_(); }

  std::vector<notes::BleNote> pending() override {
    std::vector<notes::BleNote> out;
    if (!ready()) return out;
    for (const auto& id : store_.pending_ids()) {
      notes::Meta meta;
      if (!store_.read_meta(id, meta)) continue;  // a folder without note.json: not a note
      out.push_back({meta.note_id, meta.bytes, meta.duration_s, meta.sha256, meta.recorded_at});
    }
    return out;
  }

  bool open(std::string_view id, notes::BleNote& out) override {
    close();
    if (!ready()) return false;
    // Only a *pending* note is offered: a note in sent/ is already home, and
    // one in failed/ was rejected by the server and needs a person.
    notes::NoteState state = notes::NoteState::Pending;
    if (!store_.locate(id, state) || state != notes::NoteState::Pending) return false;
    notes::Meta meta;
    if (!store_.read_meta(id, meta)) return false;
    file_ = std::fopen(store_.audio_path(notes::NoteState::Pending, id).c_str(), "rb");
    if (!file_) return false;
    out = {meta.note_id, meta.bytes, meta.duration_s, meta.sha256, meta.recorded_at};
    return true;
  }

  size_t read(uint8_t* out, size_t max) override { return file_ ? std::fread(out, 1, max, file_) : 0; }

  bool seek(uint32_t offset) override {
    return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
  }

  void close() override {
    if (file_) std::fclose(file_);
    file_ = nullptr;
  }

  bool mark_synced(std::string_view id) override {
    notes::NoteState state = notes::NoteState::Pending;
    if (!store_.locate(id, state)) return false;
    if (state == notes::NoteState::Sent) return true;  // a repeated `done`: already home
    if (state != notes::NoteState::Pending) return false;
    if (!store_.move(id, notes::NoteState::Pending, notes::NoteState::Sent)) return false;
    store_.prune_sent();
    return true;
  }

 private:
  notes::Store& store_;
  std::function<bool()> card_ready_;
  FILE* file_ = nullptr;
};

}  // namespace

void NotesModeImpl::begin(hg::App& app, UiState& ui, DeviceConfig& config, const NotesIo& io) {
  app_ = &app;
  ui_ = &ui;
  config_ = &config;
  mic_ = io.mic;
  power_ = io.power;
  sd_mount::configure(io.sd);

  // The phone's note sync, when the board has Bluetooth (notes_ble.hpp).
  ble_ = io.ble;
  if (ble_ && ble_->available()) {
    ble_source_ = std::make_unique<CardBleSource>(store_, [this] { return sd_ready_; });
    ble_sync_.begin(ble_source_.get());
  }

  // The microphone keeps feeding the live path; a recording in progress takes
  // its copy here. This task only copies it into the writer's ring: the card
  // write happens on the writer's own task, so a slow write (this card has
  // taken 1.5 s for one 640-byte chunk) no longer stalls the app loop, the
  // console's replies or the microphone's event queue.
  const bool writer_ready = writer_.begin();
  writer_.bind(&capture_);
  app.on_mic_samples_extra = [this, writer_ready](const int16_t* samples, size_t count) {
    if (!capture_.active()) return;
    if (writer_ready) writer_.push(samples, count);
    else capture_.feed(samples, count);  // no task for the ring: write here
  };

  // A note id has to stay unique across power cycles: the counter is NVS-backed
  // and the timestamp makes the id sortable.
  const std::string seq = config_->get(kSeqKey).value_or("0");
  seq_ = static_cast<uint32_t>(std::strtoul(seq.c_str(), nullptr, 10));
  sd_probe_ms_ = 0;  // 0 = the first scan may mount at once
  refresh_counts(false);
  publish_status();
  ESP_LOGI(TAG, "offline notes ready: %u pending, %u sent, %u failed", static_cast<unsigned>(counts_.pending),
           static_cast<unsigned>(counts_.synced), static_cast<unsigned>(counts_.failed));
}

bool NotesModeImpl::clock_ok() const { return std::time(nullptr) >= static_cast<time_t>(kMinPlausibleUnix); }

void NotesModeImpl::on_note_button() {
  const uint32_t now = now_ms();
  if (mode_ == Mode::Recording)
    finish_note(now, "button");
  else if (mode_ == Mode::Idle)
    start_note(now);
  // While a note uploads, a tap is ignored: the queue finishes on its own.
}

bool NotesModeImpl::on_cancel_button() {
  if (mode_ != Mode::Recording) return false;
  abort_note("button hold");
  // The line stops counting and the resting face comes back with the counts; the
  // partial note never reached note.json, so nothing on the card refers to it.
  ui_->set(UiPhase::Idle);
  refresh_counts(true);
  publish_status();
  return true;
}

void NotesModeImpl::start_note(uint32_t now) {
  if (mode_ != Mode::Idle) return;
  // A new recording is not the previous stop's story: a refusal below must not
  // answer with the last discard's reason.
  stop_error_.clear();
  stop_discarded_ = false;
  if (app_->in_turn()) {
    ESP_LOGW(TAG, "not recording a note: Hermes is in a turn");
    return;
  }
  if (!mic_) {
    ui_->set(UiPhase::Error, "No microphone");
    return;
  }
  if (!ensure_sd()) {
    ui_->set(UiPhase::Error, sd_error_.empty() ? "No SD card" : sd_error_);
    return;
  }
  // Retention first: with the card this full and nothing synced left to
  // delete, a recording would only write until the card refused and then fail
  // halfway, so the honest answer is the error instead of a broken note.
  run_retention(now, true);
  if (low_space_) {
    ui_->set(UiPhase::Error, kLowSpaceError);
    return;
  }

  // Resolve the id against the card: a reboot inside the same second must not
  // reuse a folder.
  notes::Meta meta;
  bool created = false;
  std::string error;
  for (int tries = 0; tries < 5 && !created; ++tries) {
    ++seq_;
    meta.note_id = notes::make_note_id(app_->device_id(), std::time(nullptr), seq_);
    created = capture_.start(store_, meta.note_id, config_->note_max_s(), error);
  }
  if (!created) {
    ui_->set(UiPhase::Error, error.empty() ? "Cannot write to the card" : error);
    sd_ready_ = false;
    sd_mount::release();
    return;
  }
  config_->set(kSeqKey, std::to_string(seq_));

  meta.device_id = app_->device_id();
  meta.device_name = config_->get("name").value_or("Gadget");
  meta.firmware = esp_app_get_description()->version;
  meta.created_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  meta.recorded_at = notes::iso8601_utc(std::time(nullptr));
  meta.idempotency_key = meta.note_id;
  if (power_) {
    if (auto status = power_->read(); status && status->battery_mv) meta.battery_mv = *status->battery_mv;
  }
  meta_ = std::move(meta);
  writer_.reset_dropped();
  mode_ = Mode::Recording;
  rec_started_ms_ = now;
  stop_error_.clear();
  stop_discarded_ = false;
  ui_->set(UiPhase::Recording);
  mic_->start(notes::kSampleRate);
  ESP_LOGI(TAG, "recording %s (cap %d s)", meta_.note_id.c_str(), config_->note_max_s());
}

void NotesModeImpl::finish_note(uint32_t now, const char* why) {
  (void)now;
  if (mode_ != Mode::Recording) return;
  if (mic_) mic_->stop();
  mode_ = Mode::Idle;
  stop_discarded_ = false;
  // The microphone is stopped, so nothing new is pushed: wait for the writer
  // to have the rest on the card before the file is finished. 30 s is the
  // card's own pathological worst case, not a normal wait.
  if (!writer_.drain(30000)) ESP_LOGW(TAG, "note %s: the card did not take the rest of the audio in 30 s", meta_.note_id.c_str());
  std::string error;
  const bool capped = capture_.full();
  if (!capture_.finish(meta_, error)) {
    if (capture_.discarded()) {
      // A tap that was let go, or a microphone that never delivered: nothing was
      // said and nothing is kept. finish() already removed the partial file and
      // the note folder, and no note.json is written, so no row ever reaches
      // pending/ and the uploader's no-recording sweep never sees this note.
      // This is not a card fault: the card stays mounted, the face stays on
      // Home, and the one line says why nothing was saved.
      ESP_LOGW(TAG, "note %s discarded: no audio", meta_.note_id.c_str());
      stop_error_ = "no audio";
      stop_discarded_ = true;
      ui_->set(UiPhase::Idle);
      refresh_counts(false);
      if (ui_) ui_->set_status("No audio: nothing saved");
      return;
    }
    ESP_LOGW(TAG, "note not saved (%s): %s", why, error.c_str());
    stop_error_ = error.empty() ? "the recording was not usable" : error;
    ui_->set(UiPhase::Error, error);
    sd_ready_ = false;  // the next scan probes the card again
    sd_mount::release();
    refresh_counts(true);
    return;
  }
  // note.json is the commit: until it lands the note does not exist.
  if (!store_.write_meta(meta_.note_id, meta_, error)) {
    ESP_LOGE(TAG, "note.json not written: %s", error.c_str());
    stop_error_ = "card write failed: " + error;
    store_.discard(notes::NoteState::Pending, meta_.note_id);
    ui_->set(UiPhase::Error, "Card write failed");
    sd_ready_ = false;
    sd_mount::release();
    refresh_counts(true);
    return;
  }
  ESP_LOGI(TAG, "note %s stored: %s, %u s%s", meta_.note_id.c_str(), notes::human_bytes(meta_.bytes).c_str(),
           static_cast<unsigned>(meta_.duration_s), capped ? " (cap reached)" : "");
  // The phone's half: a short advertising window so a phone in range can take
  // the note off the card without the radio running all day.
  if (ble_) ble_->note_saved();
  if (const uint32_t lost = writer_.dropped_bytes()) {
    // The ring overflowed: the card could not keep up with the microphone. The
    // audio is continuous up to the first hole and the note still plays.
    ESP_LOGW(TAG, "note %s lost %u bytes of audio: the card could not keep up", meta_.note_id.c_str(),
             static_cast<unsigned>(lost));
    writer_.discard();
  }
  refresh_counts(true);
  ui_->set(UiPhase::Idle);
  publish_status();
}

void NotesModeImpl::abort_note(const char* why) {
  if (mic_) mic_->stop();
  writer_.discard();
  if (capture_.active()) capture_.abort();
  mode_ = Mode::Idle;
  ESP_LOGW(TAG, "recording dropped: %s", why);
}

void NotesModeImpl::tick(uint32_t now) {
  // The phone first: a transfer is driven by the commands the link queued, and
  // it runs beside whatever else the board is doing (recording included).
  step_ble(now);
  if (mode_ == Mode::Recording) {
    ui_->set(UiPhase::Recording);
    // The cap, or a live turn that needs the microphone back.
    if (capture_.full()) finish_note(now, "cap");
    else if (app_->in_turn()) finish_note(now, "live turn");
    return;
  }
  if (mode_ == Mode::Syncing) {
    step_sync(now);
    return;
  }
  if (static_cast<int32_t>(now - next_scan_ms_) >= 0) {
    // Only a queued note can change between scans (the phone's transfer and
    // this device both recount at once), so a card with nothing waiting is
    // read on the slow clock.
    const uint32_t interval = (counts_.pending == 0 && counts_.failed == 0) ? kIdleScanMs : kScanMs;
    next_scan_ms_ = now + interval;
    refresh_counts(true);
  }
  if (static_cast<int32_t>(now - next_retention_ms_) >= 0) run_retention(now, false);
  // The card-full error outlives the notice ttl: keep it on the panel while it
  // stands (an unchanged line is not repainted, so this costs no refresh). A
  // phase that cleared it on the way through (an upload's Idle) gets it back.
  if (low_space_error_) {
    if (ui_->phase() == UiPhase::Idle)
      ui_->set(UiPhase::Error, kLowSpaceError);
    else
      ui_->keep_alive(now);
  }
  begin_sync(now);
}

void NotesModeImpl::begin_sync(uint32_t now) {
  if (mode_ != Mode::Idle) return;
  if (!app_->online()) return;          // nothing to reach
  if (!clock_ok()) return;              // a 1970 timestamp is legal but useless
  if (!ensure_sd()) return;
  if (retry_waiting_ && static_cast<int32_t>(now - retry_at_ms_) < 0) return;
  // Nothing waiting: do not walk pending/ on every pass (a directory read of
  // the card, up to a hundred times a second while the link is up). Every
  // writer recounts at once, and the scan above reopens this path.
  if (counts_.pending == 0) return;

  for (const auto& id : store_.pending_ids()) {
    if (id == sync_id_ && retry_waiting_) continue;  // this one is in its backoff
    notes::Meta meta;
    if (!store_.read_meta(id, meta)) continue;
    // A recording that is gone or empty (lost to a full card, or discarded
    // after note.json was committed because the ring overflowed) will not come
    // back: the note is terminal, so it moves to failed/ once instead of being
    // rescanned every pass, which warned 30 times a second and never ended.
    const uint64_t audio_bytes = store_.audio_bytes(notes::NoteState::Pending, id);
    if (notes::plan_upload(audio_bytes) != notes::QueueAction::Upload) {
      ESP_LOGW(TAG, "note %s has no recording (%s): moving it to failed/", id.c_str(),
               notes::human_bytes(audio_bytes).c_str());
      if (!store_.move(id, notes::NoteState::Pending, notes::NoteState::Failed))
        ESP_LOGW(TAG, "could not move %s to failed/", id.c_str());
      refresh_counts(true);
      continue;
    }
    // The card has no RTC and starts at 1970, so a note taken before the first
    // clock sync carries a 1970 recorded_at. It is never uploaded that way: it
    // goes out stamped with the time it is finally sent, and that stamp is
    // written back before the upload so every retry resends the same strings.
    // The note id and the idempotency_key do not move, so the server still
    // dedupes it to one job; only the wire and the vault see the real time.
    notes::Meta wire = notes::stamped_for_upload(meta, static_cast<int64_t>(std::time(nullptr)));
    if (wire.recorded_at != meta.recorded_at) {
      ESP_LOGW(TAG, "note %s was recorded at %s, before the clock was ever set; uploading it as %s",
               id.c_str(), meta.recorded_at.c_str(), wire.recorded_at.c_str());
      std::string error;
      if (!store_.write_meta(id, wire, error))
        ESP_LOGW(TAG, "note %s: cannot record the upload time: %s", id.c_str(), error.c_str());
    }
    std::string error;
    if (!uploader_.begin(wire, store_.audio_path(notes::NoteState::Pending, id), notes_url(), std::time(nullptr),
                         app_->access_token(), [this](std::string_view canonical) { return app_->sign(canonical); },
                         error)) {
      // Anything else that stops the upload from starting (no memory, a file
      // the card will not open) is waited out on the same backoff a failed
      // transfer gets: this pass runs every few tens of milliseconds, so an
      // unbacked retry is a spin, not a retry.
      sync_id_ = id;
      retry_waiting_ = true;
      attempt_ += 1;
      const uint32_t wait_ms = notes::retry_delay_ms(attempt_);
      retry_at_ms_ = now + wait_ms;
      ESP_LOGW(TAG, "cannot upload %s: %s; next try in %u ms", id.c_str(), error.c_str(),
               static_cast<unsigned>(wait_ms));
      continue;
    }
    meta_ = wire;
    sync_id_ = id;
    attempt_ = wire.attempts;
    retry_waiting_ = false;
    mode_ = Mode::Syncing;
    ui_->set(UiPhase::Uploading);
    ESP_LOGI(TAG, "uploading %s (%s)", id.c_str(), notes::human_bytes(meta.bytes).c_str());
    return;
  }
}

void NotesModeImpl::step_sync(uint32_t now) {
  const auto step = uploader_.step();
  if (step == notes::Uploader::Step::Running) {
    // Keep the very long ones visibly moving, without a republish every pass.
    const uint32_t total = uploader_.total();
    if (total) {
      const unsigned pct = static_cast<unsigned>(uploader_.sent() * 100ull / total);
      if (pct / 10 != last_pct_ / 10) {
        last_pct_ = pct;
        ui_->set(UiPhase::Uploading, std::to_string(pct) + "%");
      }
    }
    return;
  }

  const std::string id = sync_id_;
  mode_ = Mode::Idle;
  sync_id_.clear();
  if (id.empty()) {
    retry_waiting_ = false;
    return;
  }
  if (step == notes::Uploader::Step::Done) {
    // Acknowledged (202), or the server already had this key (200): exact-once
    // ends here and the folder moves out of pending/.
    if (!store_.move(id, notes::NoteState::Pending, notes::NoteState::Sent))
      ESP_LOGW(TAG, "%s synced but could not be moved to sent/", id.c_str());
    store_.prune_sent();
    attempt_ = 0;
    retry_waiting_ = false;
    refresh_counts(true);
    ui_->set(UiPhase::Idle);
    publish_status();
    ESP_LOGI(TAG, "note %s synced (HTTP %d)", id.c_str(), uploader_.http_status());
    return;
  }
  if (step == notes::Uploader::Step::Rejected) {
    ESP_LOGW(TAG, "note %s rejected (HTTP %d): %s", id.c_str(), uploader_.http_status(), uploader_.error().c_str());
    if (!store_.move(id, notes::NoteState::Pending, notes::NoteState::Failed))
      ESP_LOGW(TAG, "%s rejected but could not be moved to failed/", id.c_str());
    retry_waiting_ = false;
    attempt_ = 0;
    refresh_counts(true);
    ui_->set(UiPhase::Idle);
    publish_status();
    return;
  }

  // Retry: the note stays pending, the backoff doubles from 5 s to 5 min.
  retry_waiting_ = true;
  retry_at_ms_ = now + notes::retry_delay_ms(attempt_ + 1);
  attempt_ += 1;
  notes::Meta meta = uploader_.meta();
  meta.attempts = attempt_;
  std::string error;
  store_.write_meta(id, meta, error);
  ESP_LOGW(TAG, "note %s: %s; attempt %u, retrying in %u ms", id.c_str(), uploader_.error().c_str(),
           static_cast<unsigned>(attempt_), static_cast<unsigned>(notes::retry_delay_ms(attempt_)));
  ui_->set(UiPhase::Idle);
  publish_status();
}

void NotesModeImpl::step_ble(uint32_t now) {
  if (!ble_ || !ble_source_ || !ble_->available()) return;

  if (!ble_->connected()) {
    if (ble_linked_) {
      ble_linked_ = false;
      // The note stays in pending/: the phone resumes from the offset it
      // acknowledged, so a dropped link costs the bytes in flight, nothing more.
      ble_sync_.disconnected();
      refresh_counts(true);
      publish_status();
    }
    return;
  }
  ble_linked_ = true;
  ble_sync_.set_mtu(ble_->mtu());

  std::string line;
  while (ble_->poll_command(line)) {
    // A phone asking for notes is also the cue to probe the card again: the
    // counts scan runs every 5 s and the phone may be quicker than that.
    if (!sd_ready_) ensure_sd();
    ESP_LOGI(TAG, "phone: %s", line.c_str());
    ble_sync_.command(line, now);
  }
  ble_sync_.tick(now, [this](const uint8_t* data, size_t len) { return ble_->send_frame(data, len); });

  if (ble_sync_.took_delivery()) {
    // The phone stored the note and Hermes took it: the queue is one shorter.
    ESP_LOGI(TAG, "note delivered over Bluetooth");
    retry_waiting_ = false;
    refresh_counts(true);
    publish_status();
  }

  const auto& progress = ble_sync_.progress();
  if (progress.active) {
    // An e-paper panel flashes on every repaint: once per 10% is enough to see
    // it move, and the status line keeps the counts otherwise.
    if (progress.pct / 10 != ble_pct_ / 10) {
      ble_pct_ = progress.pct;
      publish_status();
    }
  } else if (ble_pct_ != 100) {
    ble_pct_ = 100;
    if (!progress.error.empty()) ESP_LOGW(TAG, "Bluetooth transfer stopped: %s", progress.error.c_str());
    refresh_counts(true);
    publish_status();
  }
}

void NotesModeImpl::run_retention(uint32_t now, bool force) {
  next_retention_ms_ = now + kRetentionMs;
  // Never during a format, and never while this mode holds a note open: a
  // recording and an upload both live in pending/, which the rule never
  // touches, but a scan is card I/O the device does not need at that moment.
  if (formatting_ || mode_ != Mode::Idle) return;
  if (!ensure_sd()) {
    low_space_ = false;
    if (low_space_error_) {
      // The card left: "almost full" is no longer the truth about this device.
      low_space_error_ = false;
      ui_->clear_error();
      publish_status();
    }
    // The card's own retry clock, not the five-minute one: a card put back in
    // should get its pass as soon as the mount answers again.
    next_retention_ms_ = now + kSdRetryMs;
    return;
  }

  notes::CardSpace card;
  uint64_t total = 0, free_bytes = 0;
  if (sd_mount::space(&total, &free_bytes)) {
    card.total = total;
    card.free = free_bytes;
  }
  // Idle is guaranteed above, so nothing is recording or uploading and the
  // unsynced folders are what stays: only sent/ is a candidate.
  notes::RetentionResult result =
      notes::enforce(store_, card, static_cast<int64_t>(std::time(nullptr)), retention_);
  if (result.deleted) {
    ESP_LOGI(TAG, "retention%s: deleted %u synced notes (%u older than %u days, %u for space), %s freed",
             force ? " (asked)" : "", static_cast<unsigned>(result.deleted),
             static_cast<unsigned>(result.deleted_for_age),
             static_cast<unsigned>(retention_.max_age_s / 86400),
             static_cast<unsigned>(result.deleted_for_space),
             notes::human_bytes(result.deleted_bytes).c_str());
    refresh_counts(true);
  } else if (result.evaluated) {
    // The first pass after boot says out loud that the rule ran and found
    // nothing; later quiet passes stay at debug so a day of them cannot push the
    // boot lines out of the log ring.
    if (!retention_logged_) {
      retention_logged_ = true;
      ESP_LOGI(TAG, "retention: nothing to delete (%d%% free, %u unsynced kept)",
               result.free_pct_before, static_cast<unsigned>(result.unsynced_left));
    } else {
      ESP_LOGD(TAG, "retention: nothing to delete (%d%% free, %u unsynced kept)", result.free_pct_before,
               static_cast<unsigned>(result.unsynced_left));
    }
  }

  // The rule works on the sizes it removed; the card is the authority on what
  // is actually left (clusters round up, a directory entry costs a cluster of
  // its own). One more query, then the screen says the truth either way.
  int pct = result.free_pct_after;
  bool low = result.low_space;
  uint64_t total_now = 0, free_now = 0;
  if (sd_mount::space(&total_now, &free_now) && total_now) {
    pct = static_cast<int>(free_now * 100ull / total_now);
    low = pct < retention_.min_free_pct;
  }
  result.low_space = low;
  result.free_pct_after = pct;
  retention_last_ = result;
  low_space_ = low;
  if (low) {
    if (!low_space_error_)
      ESP_LOGW(TAG, "card %d%% free and no synced notes left to delete: showing the error", pct);
    low_space_error_ = true;
    ui_->set(UiPhase::Error, kLowSpaceError);
    next_retention_ms_ = now + kRetentionLowMs;
  } else if (low_space_error_) {
    low_space_error_ = false;
    ui_->clear_error();
    publish_status();
    ESP_LOGI(TAG, "card %d%% free again: the retention error is cleared", pct);
  }
}

void NotesModeImpl::refresh_counts(bool publish) {
  if (!ensure_sd()) {
    if (publish) publish_status();
    return;
  }
  const uint32_t pending = store_.count(notes::NoteState::Pending);
  const uint32_t sent = store_.count(notes::NoteState::Sent);
  const uint32_t failed = store_.count(notes::NoteState::Failed);
  counts_.pending = pending;
  counts_.synced = sent;
  counts_.failed = failed;
  counts_.stored = pending + sent + failed;
  // A waiting note is the reason to advertise: the port opens a window for it
  // (and keeps trying on its own clock while the count stays above zero).
  if (ble_) ble_->pending(pending);
  queued_bytes_ = store_.pending_bytes();
  refresh_space();
  refresh_newest();
  if (publish) publish_status();
}

// How much room the card has, for the resting screen's footer. FAT answers this
// from its own free-cluster table: it costs one walk of the table, so it is read
// with the counts scan (every 5 s) rather than per frame, and a card that refuses
// the question (pulled mid-scan, or unmounted) simply reports nothing.
void NotesModeImpl::refresh_space() {
  uint64_t total = 0, free_bytes = 0;
  if (esp_vfs_fat_info(kSdRoot, &total, &free_bytes) != ESP_OK || total == 0) {
    sd_free_known_ = false;
    return;
  }
  sd_total_bytes_ = total;
  sd_free_bytes_ = free_bytes;
  sd_free_known_ = true;
}

// The newest note on the card: the resting status screen says which note it is
// and how long it ran. The ids sort by time (`<device>-YYYYMMDDTHHMMSS-<seq>`),
// so the scan needs no meta read for everything it skips, and it re-reads only
// when the newest id changes.
void NotesModeImpl::refresh_newest() {
  std::string newest;
  for (notes::NoteState state : {notes::NoteState::Pending, notes::NoteState::Sent}) {
    for (const auto& id : store_.list(state)) {
      if (id > newest) newest = id;
    }
  }
  if (newest == newest_id_) return;
  newest_id_ = newest;
  newest_valid_ = false;
  newest_duration_s_ = 0;
  notes::Meta meta;
  if (newest.empty() || !store_.read_meta(newest, meta)) return;
  newest_duration_s_ = meta.duration_s;
  // created_ms is the wall clock at record time; a note from before the first
  // sync reads 1970, which is not a time the screen may print.
  const int64_t seconds = meta.created_ms / 1000;
  struct tm local {};
  const time_t when = static_cast<time_t>(seconds);
  if (seconds < notes::kMinTrustedStampUnix || localtime_r(&when, &local) == nullptr) return;
  newest_valid_ = true;
  newest_hour_ = local.tm_hour;
  newest_minute_ = local.tm_min;
}

NoteStatus NotesModeImpl::status() const {
  NoteStatus s;
  s.pending = counts_.pending;
  s.synced = counts_.synced;
  s.failed = counts_.failed;
  s.stored = counts_.stored;
  s.sd_ready = sd_ready_;
  s.sd_free_bytes = sd_free_bytes_;
  s.sd_total_bytes = sd_total_bytes_;
  s.sd_free_known = sd_free_known_ && sd_ready_;
  s.has_newest = !newest_id_.empty();
  s.newest_valid = newest_valid_;
  s.newest_hour = newest_hour_;
  s.newest_minute = newest_minute_;
  s.newest_duration_s = newest_duration_s_;
  s.error = ui_ != nullptr && ui_->phase() == UiPhase::Error ? ui_->detail() : std::string();

  switch (mode_) {
    case Mode::Recording:
      s.phase = NoteStatus::Phase::Recording;
      s.recording_s = (now_ms() - rec_started_ms_) / 1000;
      break;
    case Mode::Syncing: {
      s.phase = NoteStatus::Phase::Uploading;
      s.uploading_bytes = meta_.bytes;
      const int64_t seconds = meta_.created_ms / 1000;
      struct tm local {};
      const time_t when = static_cast<time_t>(seconds);
      if (seconds >= notes::kMinTrustedStampUnix && localtime_r(&when, &local) != nullptr) {
        s.uploading_valid = true;
        s.uploading_hour = local.tm_hour;
        s.uploading_minute = local.tm_min;
      }
      break;
    }
    case Mode::Idle:
      s.phase = NoteStatus::Phase::Idle;
      break;
  }
  // A phone taking a note over Bluetooth is the queue uploading over another
  // link: the resting screen shows the same busy state it shows for a POST
  // (notes_screen.cpp), so a transfer is visible without a second widget.
  const auto& ble = ble_sync_.progress();
  if (ble.active && s.phase == NoteStatus::Phase::Idle) {
    s.phase = NoteStatus::Phase::Uploading;
    s.uploading_bytes = ble.total;
  }
  // A failure that still stands is the state, whatever the mode is doing now.
  if (s.phase == NoteStatus::Phase::Idle && !s.error.empty()) s.phase = NoteStatus::Phase::Error;
  return s;
}

std::string NotesModeImpl::status_line() const {
  if (mode_ == Mode::Recording) return {};  // the Recording phase owns the line
  char buf[64];
  // A phone taking a note over Bluetooth owns the line while it does: that is
  // the one thing on this screen a person has to be able to watch.
  const auto& ble = ble_sync_.progress();
  if (ble.active) {
    std::snprintf(buf, sizeof(buf), "Bluetooth %u%%", static_cast<unsigned>(ble.pct));
    return buf;
  }
  if (counts_.failed)
    std::snprintf(buf, sizeof(buf), "%u pending, %u failed", static_cast<unsigned>(counts_.pending),
                  static_cast<unsigned>(counts_.failed));
  else if (counts_.pending)
    std::snprintf(buf, sizeof(buf), "%u pending, %u sent", static_cast<unsigned>(counts_.pending),
                  static_cast<unsigned>(counts_.synced));
  else if (counts_.synced)
    std::snprintf(buf, sizeof(buf), "%u notes synced", static_cast<unsigned>(counts_.synced));
  else if (!sd_ready_)
    std::snprintf(buf, sizeof(buf), "No SD card");
  else
    std::snprintf(buf, sizeof(buf), "No notes");
  // A phone that is connected and idle is worth a mark: it means the notes on
  // the card can leave over Bluetooth without a network.
  std::string line = buf;
  if (ble_ && ble_->available() && ble_->connected()) line += " · BT";
  return line;
}

void NotesModeImpl::publish_status() {
  if (!ui_) return;
  ui_->set_status(status_line());
}

bool NotesModeImpl::ensure_sd() {
  if (sd_ready_) return true;
  const uint32_t now = now_ms();
  // A missing card is the normal state: probe on the retry clock, not on
  // every pass of the app loop.
  if (sd_probe_ms_ != 0 && static_cast<int32_t>(now - sd_probe_ms_) < static_cast<int32_t>(kSdRetryMs)) return false;
  sd_probe_ms_ = now == 0 ? 1 : now;
  std::string error;
  if (!sd_mount::ensure(false, &error)) {
    sd_error_ = error.empty() ? "No SD card" : error;
    sd_free_known_ = false;  // nothing on the glass may quote a card that is gone
    return false;
  }
  if (!store_.ensure()) {
    sd_error_ = "The card is not writable";
    sd_free_known_ = false;
    return false;
  }
  sd_ready_ = true;
  sd_error_.clear();
  return true;
}

std::string NotesModeImpl::notes_url() const {
  // `notes_url` overrides everything (a host behind a different port or a
  // proxy). Otherwise the hub's own host, on the notes port.
  if (auto stored = config_->get(kNotesUrlKey); stored && !stored->empty()) return *stored;
  std::string base = app_->server_url();  // ws(s)://host[:port]/path
  const size_t scheme = base.find("://");
  if (scheme == std::string::npos) return {};
  std::string host = base.substr(scheme + 3);
  const size_t slash = host.find('/');
  if (slash != std::string::npos) host.resize(slash);
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) host.resize(colon);
  if (host.empty()) return {};
  char buf[128];
  std::snprintf(buf, sizeof(buf), "http://%s:%u/notes/v1", host.c_str(), static_cast<unsigned>(kDefaultNotesPort));
  return buf;
}

std::string NotesModeImpl::console(std::string_view args) {
  const std::string line = trim(args);
  const size_t sp = line.find(' ');
  const std::string cmd = line.substr(0, sp);
  const std::string rest = sp == std::string::npos ? std::string() : trim(std::string_view(line).substr(sp + 1));

  if (cmd.empty() || cmd == "status") {
    char buf[320];
    const auto& ble = ble_sync_.progress();
    std::string ble_state = "off";
    if (ble_ && ble_->available()) {
      char b[40];
      if (ble.active) std::snprintf(b, sizeof(b), "%u%%", static_cast<unsigned>(ble.pct));
      else std::snprintf(b, sizeof(b), "%s", ble_ && ble_->connected() ? "idle" : "away");
      ble_state = b;
    }
    std::snprintf(buf, sizeof(buf),
                  "@notes stored %u pending %u sent %u failed %u | card %s | %s | mode %s | %u B queued | ble %s",
                  static_cast<unsigned>(counts_.stored), static_cast<unsigned>(counts_.pending),
                  static_cast<unsigned>(counts_.synced), static_cast<unsigned>(counts_.failed),
                  sd_ready_ ? "ok" : (sd_error_.empty() ? "down" : sd_error_.c_str()),
                  notes_url().empty() ? "no url" : notes_url().c_str(),
                  mode_ == Mode::Recording ? "recording" : mode_ == Mode::Syncing ? "syncing" : "idle",
                  static_cast<unsigned>(queued_bytes_), ble_state.c_str());
    std::string out = buf;
    // Notes recorded before the board ever had a clock (their id says 1970):
    // they upload stamped with the time they are finally sent, and this is how
    // many are still waiting for that.
    if (counts_.pending && sd_ready_) {
      unsigned undated = 0;
      for (const auto& id : store_.list(notes::NoteState::Pending)) {
        if (notes::note_id_undated(id)) ++undated;
      }
      if (undated) out += " | " + std::to_string(undated) + " undated";
    }
    return out;
  }
  if (cmd == "record" || cmd == "start") {
    if (mode_ == Mode::Recording) return "@ok already recording";
    start_note(now_ms());
    if (mode_ == Mode::Recording) return "@ok recording " + meta_.note_id;
    if (low_space_) return std::string("@error ") + kLowSpaceError;
    if (!stop_error_.empty()) return "@error " + stop_error_;
    return "@error cannot record (see log)";
  }
  if (cmd == "stop") {
    if (mode_ != Mode::Recording) return "@error not recording";
    const std::string id = meta_.note_id;
    finish_note(now_ms(), "console");
    if (mode_ == Mode::Recording) return "@error the note was not saved";
    // A capture discarded for having no audio is not a failed note: nothing was
    // kept, and the card was never at fault.
    if (stop_discarded_) return notes::discarded_answer(id);
    // finish_note leaves the mode Idle on every path, so the reason it stored
    // nothing is what says whether the console may claim a note.
    if (!stop_error_.empty()) return "@error the note was not saved: " + stop_error_;
    return "@ok stored " + id;
  }
  if (cmd == "sync") {
    retry_waiting_ = false;
    retry_at_ms_ = 0;
    if (mode_ != Mode::Idle) return "@ok already syncing";
    if (!app_->online()) return "@error Hermes is not reachable yet";
    if (!clock_ok()) return "@error the clock is not set yet";
    begin_sync(now_ms());
    return mode_ == Mode::Syncing ? "@ok uploading " + sync_id_ : "@ok nothing pending";
  }
  if (cmd == "list") {
    // `<id>=<seconds>`: each note's own duration_seconds, so a note past 100
    // minutes reads its real length and never a formatted clamp.
    std::string out = "@notes";
    for (notes::NoteState s : {notes::NoteState::Pending, notes::NoteState::Sent, notes::NoteState::Failed}) {
      out += std::string(" ") + notes::state_name(s) + "=";
      const auto ids = store_.list(s);
      out += std::to_string(ids.size());
      // The newest ids: the 8-item cap exists to keep one console line readable,
      // and taking them from the front of an ascending queue hid the note just
      // recorded. The counts above still say how many there are in total.
      const size_t shown = ids.size() < 8 ? ids.size() : 8;
      for (size_t i = ids.size(); i > ids.size() - shown; --i) {
        const std::string& id = ids[i - 1];
        notes::Meta meta;
        out += " " + id;
        if (store_.read_meta(id, meta)) out += "=" + std::to_string(meta.duration_s);
      }
    }
    return out;
  }
  if (cmd == "delete" || cmd == "rm") {
    if (rest.empty()) return "@error usage: notes delete <note_id> | notes delete --failed";
    if (!ensure_sd()) return "@error " + (sd_error_.empty() ? std::string("no card") : sd_error_);
    if (rest == "--failed") {
      // failed/ is terminal: nothing is uploading those, so the folder empties
      // even while a new note records.
      const uint32_t removed = store_.erase_failed();
      refresh_counts(true);  // repaints the counts line
      return notes::deleted_failed_answer(removed);
    }
    // The two notes that are in use stay: the recording being written, and the
    // one the uploader holds open (its retry backoff reopens the same folder,
    // so it counts as uploading until it lands in sent/ or failed/).
    if (mode_ == Mode::Recording && rest == meta_.note_id) return "@error the note is recording";
    if (!sync_id_.empty() && rest == sync_id_) return "@error the note is uploading";
    notes::NoteState from = notes::NoteState::Pending;
    if (!store_.erase(rest, &from)) return notes::unknown_note_answer(rest);
    refresh_counts(true);  // repaints the counts line
    return notes::deleted_answer(rest);
  }
  if (cmd == "retention") {
    // The console asks for a pass now. It is the same rule the device runs by
    // itself, read back from what it did rather than from what it found.
    if (!ensure_sd()) return "@error " + (sd_error_.empty() ? std::string("no card") : sd_error_);
    if (mode_ != Mode::Idle) return "@error busy: a note is recording or uploading";
    run_retention(now_ms(), true);
    const auto& r = retention_last_;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "@ok retention deleted %u (%u age, %u space), %s freed | free %d%% | sent %u/pending %u/failed %u",
                  static_cast<unsigned>(r.deleted), static_cast<unsigned>(r.deleted_for_age),
                  static_cast<unsigned>(r.deleted_for_space), notes::human_bytes(r.deleted_bytes).c_str(),
                  r.free_pct_after, static_cast<unsigned>(counts_.synced),
                  static_cast<unsigned>(counts_.pending), static_cast<unsigned>(counts_.failed));
    return buf;
  }
  if (cmd == "mount") {
    sd_ready_ = false;
    sd_probe_ms_ = 0;
    return ensure_sd() ? "@ok card mounted" : "@error " + sd_error_;
  }
  if (cmd == "format") {
    if (mode_ == Mode::Recording) return "@error stop the recording first";
    if (formatting_) return "@ok already formatting";
    if (mode_ == Mode::Syncing) uploader_.cancel();
    mode_ = Mode::Idle;
    // A format blocks the SD mount for tens of seconds; run it off the app
    // task so the screen, the buttons and the console stay alive.
    sd_mount::release();
    formatting_ = true;
    const BaseType_t started = xTaskCreate(
        [](void* arg) {
          auto* self = static_cast<NotesModeImpl*>(arg);
          std::string error;
          const bool ok = sd_mount::ensure(true, &error);
          self->sd_ready_ = ok;
          self->sd_probe_ms_ = 0;
          if (ok) self->store_.ensure();
          self->sd_error_ = ok ? std::string() : (error.empty() ? "format failed" : error);
          self->formatting_ = false;
          ESP_LOGI(TAG, "format %s", ok ? "done" : self->sd_error_.c_str());
          vTaskDelete(nullptr);
        },
        "hg-notes-fmt", 4096, this, 4, nullptr);
    if (started != pdPASS) {
      formatting_ = false;
      return "@error no task for the format";
    }
    return "@ok formatting the card";
  }
  if (cmd == "ble") {
    if (!ble_ || !ble_->available()) return "@error this build has no Bluetooth note sync";
    const auto& p = ble_sync_.progress();
    char buf[240];
    if (p.active)
      std::snprintf(buf, sizeof(buf), "@notes ble link %s mtu %u transfer %s %u%% (%u/%u B)",
                    ble_->connected() ? "up" : "down", static_cast<unsigned>(ble_->mtu()), p.id.c_str(),
                    static_cast<unsigned>(p.pct), static_cast<unsigned>(p.sent), static_cast<unsigned>(p.total));
    else if (!p.error.empty())
      std::snprintf(buf, sizeof(buf), "@notes ble link %s mtu %u idle, last transfer stopped: %s",
                    ble_->connected() ? "up" : "down", static_cast<unsigned>(ble_->mtu()), p.error.c_str());
    else
      std::snprintf(buf, sizeof(buf), "@notes ble link %s mtu %u idle",
                    ble_->connected() ? "up" : "down", static_cast<unsigned>(ble_->mtu()));
    return buf;
  }
  if (cmd == "prune") {
    if (mode_ == Mode::Recording) return "@error stop the recording first";
    const notes::Store::PruneCounts removed = store_.prune_notes();
    refresh_counts(true);
    return notes::pruned_answer(removed.sent, removed.pending, removed.failed);
  }
  return "@error usage: notes [status | record | stop | sync | ble | list | mount | format | prune | retention | delete]";
}

}  // namespace hgp
