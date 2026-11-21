// See notes_mode.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/notes_mode.hpp"

#include <cstdio>
#include <ctime>

#include "device_config.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
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
constexpr uint32_t kSdRetryMs = 30000;
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
  // its copy here, on the same task.
  app.on_mic_samples_extra = [this](const int16_t* samples, size_t count) {
    if (capture_.active()) capture_.feed(samples, count);
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

void NotesModeImpl::start_note(uint32_t now) {
  if (mode_ != Mode::Idle) return;
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
  mode_ = Mode::Recording;
  rec_started_ms_ = now;
  stop_error_.clear();
  ui_->set(UiPhase::Recording);
  mic_->start(notes::kSampleRate);
  ESP_LOGI(TAG, "recording %s (cap %d s)", meta_.note_id.c_str(), config_->note_max_s());
}

void NotesModeImpl::finish_note(uint32_t now, const char* why) {
  (void)now;
  if (mode_ != Mode::Recording) return;
  if (mic_) mic_->stop();
  mode_ = Mode::Idle;
  std::string error;
  const bool capped = capture_.full();
  if (!capture_.finish(meta_, error)) {
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
  refresh_counts(true);
  ui_->set(UiPhase::Idle);
  publish_status();
}

void NotesModeImpl::abort_note(const char* why) {
  if (mic_) mic_->stop();
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
    next_scan_ms_ = now + kScanMs;
    refresh_counts(true);
  }
  begin_sync(now);
}

void NotesModeImpl::begin_sync(uint32_t now) {
  if (mode_ != Mode::Idle) return;
  if (!app_->online()) return;          // nothing to reach
  if (!clock_ok()) return;              // a 1970 timestamp is legal but useless
  if (!ensure_sd()) return;
  if (retry_waiting_ && static_cast<int32_t>(now - retry_at_ms_) < 0) return;

  for (const auto& id : store_.pending_ids()) {
    if (id == sync_id_ && retry_waiting_) continue;  // this one is in its backoff
    notes::Meta meta;
    if (!store_.read_meta(id, meta)) continue;
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
      ESP_LOGW(TAG, "cannot upload %s: %s", id.c_str(), error.c_str());
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
  queued_bytes_ = store_.pending_bytes();
  refresh_newest();
  if (publish) publish_status();
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
    return false;
  }
  if (!store_.ensure()) {
    sd_error_ = "The card is not writable";
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
    return mode_ == Mode::Recording ? "@ok recording " + meta_.note_id : "@error cannot record (see log)";
  }
  if (cmd == "stop") {
    if (mode_ != Mode::Recording) return "@error not recording";
    const std::string id = meta_.note_id;
    finish_note(now_ms(), "console");
    if (mode_ == Mode::Recording) return "@error the note was not saved";
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
    std::string out = "@notes";
    for (notes::NoteState s : {notes::NoteState::Pending, notes::NoteState::Sent, notes::NoteState::Failed}) {
      out += std::string(" ") + notes::state_name(s) + "=";
      const auto ids = store_.list(s);
      out += std::to_string(ids.size());
      for (size_t i = 0; i < ids.size() && i < 8; ++i) out += " " + ids[i];
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
    const uint32_t removed = store_.prune_sent();
    refresh_counts(true);
    return "@ok pruned " + std::to_string(removed);
  }
  return "@error usage: notes [status | record | stop | sync | ble | list | mount | format | prune | delete]";
}

}  // namespace hgp
