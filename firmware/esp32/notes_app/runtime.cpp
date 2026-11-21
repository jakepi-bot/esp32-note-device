// See runtime.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "notes_app/runtime.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "battery_curve.hpp"
#include "device_config.hpp"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "hg/crypto.hpp"
#include "hg/protocol.hpp"
#include "notes/sd_mount.hpp"
#include "notes_app/ui_keys.hpp"

namespace hgn {
namespace {

// The card's mount owner, spelled short: this module is its only user here.
namespace sd_mount = hgp::sd_mount;

const char* TAG = "hg.notes";

// How often the card is scanned for the counts, and how often a mount is
// retried while no card answers.
constexpr uint32_t kScanMs = 5000;
constexpr uint32_t kIdleScanMs = 30000;  // nothing queued: a resting card is not walked every 5 s
constexpr uint32_t kSdRetryMs = 30000;
// The retention rule's own clocks, as on the Hermes build.
constexpr uint32_t kRetentionMs = 300000;
constexpr uint32_t kRetentionLowMs = 60000;
constexpr uint32_t kBatteryMs = 30000;
// How long a Wi-Fi window stays open unless it finishes early (plan section 4:
// boot with work, a note just recorded, `Sync now`, an unset clock).
constexpr uint32_t kWindowMs = 120000;
// How often a closed window comes back while there is still work for it. Fifteen
// minutes keeps the duty cycle honest (two minutes up, a quarter hour down)
// without leaving a note on the card until the next reboot.
constexpr uint32_t kNetRetryMs = 900000;
// 2020-01-01: an earlier system clock means no clock has been set yet.
constexpr int64_t kMinPlausibleUnix = 1577836800;
constexpr const char* kLowSpaceError = "SD almost full, no synced notes to delete";
constexpr const char* kSeqKey = "note_seq";
constexpr const char* kNotesUrlKey = "notes_url";
constexpr uint16_t kDefaultNotesPort = 8766;
// The Discharge curve this board has none of: 3.30 V empty, 4.15 V full.
constexpr uint16_t kEmptyMv = 3300, kFullMv = 4150;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool clock_ok() { return std::time(nullptr) >= static_cast<time_t>(kMinPlausibleUnix); }

std::string trim(std::string_view text) {
  size_t b = 0, e = text.size();
  while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
  while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
  return std::string(text.substr(b, e - b));
}

// The card as the Bluetooth transfer reads it (notes_ble.hpp): pending/ notes
// only, and a note moves to sent/ only when the phone says Hermes took it.
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
      if (!store_.read_meta(id, meta)) continue;
      out.push_back({meta.note_id, meta.bytes, meta.duration_s, meta.sha256, meta.recorded_at});
    }
    return out;
  }

  bool open(std::string_view id, notes::BleNote& out) override {
    close();
    if (!ready()) return false;
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

// ---------------------------------------------------------------------------
// Boot

bool Runtime::begin(const Io& io) {
  io_ = io;
  if (!io_.storage || !io_.config) {
    ESP_LOGE(TAG, "no storage: there is no device identity to upload with");
    return false;
  }
  sd_mount::configure(io_.sd);
  if (!load_identity()) return false;

  // The phone's note sync, when the board has Bluetooth (notes_ble.hpp).
  if (io_.ble && io_.ble->available()) {
    ble_source_ = std::make_unique<CardBleSource>(store_, [this] { return sd_ready_; });
    ble_sync_.begin(ble_source_.get());
  }
  writer_ready_ = writer_.begin();
  writer_.bind(&capture_);
  if (!writer_ready_) ESP_LOGW(TAG, "no writer task: a recording writes its audio on the app task");

  ensure_sd();
  refresh_counts();
  refresh_battery();
  ready_ = true;
  ESP_LOGI(TAG, "runtime ready: device %s, %u pending, %u sent, %u failed", device_id_.c_str(),
           static_cast<unsigned>(pending_), static_cast<unsigned>(sent_), static_cast<unsigned>(failed_));

  // A boot with something to do opens the first window itself; an unset clock
  // always does, because every note recorded before it is stamped 1970.
  // A failed note (rejected by the server) is a person's work, not the radio's,
  // so it opens no window.
  if (pending_ > 0 || !clock_ok()) open_net(now_ms(), clock_ok() ? Net::Boot : Net::Clock);
  return true;
}

bool Runtime::load_identity() {
  auto stored = io_.storage->get("device_key");
  if (stored) {
    if (!hg::crypto::base64_decode(*stored, key_) || key_.size() != 32) key_.clear();
  }
  if (key_.size() != 32) {
    // First boot: the key this device signs with, kept the way the Hermes app
    // keeps it (`device_key`, base64, generated once).
    key_.assign(32, 0);
    esp_fill_random(key_.data(), key_.size());
    if (!io_.storage->set("device_key", hg::crypto::base64_encode(key_.data(), key_.size())))
      ESP_LOGW(TAG, "the device key was not stored: every boot makes a new one");
  }
  device_id_ = hg::proto::device_id_for_key(key_.data(), key_.size());
  token_ = io_.storage->get("token").value_or("");
  device_name_ = io_.storage->get("name").value_or(CONFIG_HG_DEFAULT_NAME);
  return true;
}

// ---------------------------------------------------------------------------
// Power

void Runtime::refresh_battery() {
  if (!io_.power) {
    battery_pct_ = -1;
    return;
  }
  const auto status = io_.power->read();
  if (!status) {
    battery_pct_ = -1;
    return;
  }
  charging_ = status->charging.value_or(false);
  external_ = status->external_power.value_or(false);
  if (status->battery_mv) battery_mv_ = *status->battery_mv;
  if (status->battery_percent) {
    battery_pct_ = *status->battery_percent;
    return;
  }
  if (!status->battery_mv) {
    battery_pct_ = -1;
    return;
  }
  const uint16_t mv = *status->battery_mv;
  if (io_.battery_curve) {
    battery_pct_ = hg::percent_on_curve(io_.battery_curve, mv);
    return;
  }
  if (mv <= kEmptyMv) {
    battery_pct_ = 0;
  } else if (mv >= kFullMv) {
    battery_pct_ = 100;
  } else {
    battery_pct_ = static_cast<int>((mv - kEmptyMv) * 100u / (kFullMv - kEmptyMv));
  }
}

void Runtime::shutdown() {
  const uint32_t now = now_ms();
  ESP_LOGW(TAG, "power off asked: flushing the card, closing the radio, dropping the latch");
  // A recording becomes a note here: its note.json is the commit, and a partial
  // one that never reached it is the only thing that may be lost.
  if (mode_ == Mode::Recording) finish_note(now, "power off");
  // An upload in flight stays pending/: cancelled now, resent from the top on
  // the next boot, which is what the queue's idempotency key is for.
  if (mode_ == Mode::Syncing) uploader_.cancel();
  mode_ = Mode::Idle;
  close_net(now);
  if (io_.wifi && io_.wifi->setup_open()) io_.wifi->stop_setup();
  if (io_.wifi && io_.wifi->radio_running()) io_.wifi->radio_off();
  // Nothing holds the card now: the writer drained in finish_note, the files
  // are closed, the uploader let go of its handle.
  if (sd_ready_ || sd_mount::mounted()) sd_mount::release();
  sd_ready_ = false;
  ready_ = false;

  // On USB the latch is not what feeds the board: `LatchPower::power_off()`
  // writes the pin and returns true, and on this board the card sits on the
  // latched rail, so dropping it there kills the card and leaves the device
  // running without storage. A shutdown with external power therefore flushes
  // and reports, and the latch stays up (a person unplugs it).
  const bool external = io_.power && io_.power->external_power().value_or(false);
  if (external) ESP_LOGW(TAG, "external power: the latch is not the supply, so the board stays on");
  if (!io_.power || external || !io_.power->power_off()) {
    // On USB the latch is not the supply: the board keeps running with the card
    // unmounted, which mounts nothing. Come back all the way instead.
    ESP_LOGW(TAG, "still powered over USB: mounting the card again");
    std::string error;
    sd_ready_ = sd_mount::ensure(false, &error) && store_.ensure();
    if (!sd_ready_) ESP_LOGE(TAG, "the card did not come back: %s", error.empty() ? "no answer" : error.c_str());
    ready_ = true;
  }
}

// ---------------------------------------------------------------------------
// Network windows

void Runtime::open_net(uint32_t now, Net why) {
  const uint32_t until = now + kWindowMs;
  if (net_ == Net::Closed || static_cast<int32_t>(until - net_until_ms_) > 0) net_until_ms_ = until;
  net_ = why;
  net_was_up_ = false;
  ESP_LOGI(TAG, "Wi-Fi window open (%s) for %u s", net_reason(),
           static_cast<unsigned>((net_until_ms_ - now) / 1000));
}

bool Runtime::net_open(uint32_t now) const {
  // A phone's setup page holds the station up whatever the window is doing: it
  // is the only way to give the device a network when none is saved, and it has
  // its own deadline in the port.
  if (setup_window()) return true;
  if (net_ == Net::Closed) return false;
  return static_cast<int32_t>(net_until_ms_ - now) > 0;
}

void Runtime::close_net(uint32_t now) {
  if (net_ == Net::Closed) return;
  ESP_LOGI(TAG, "Wi-Fi window closed (%s): %u pending, clock %s", net_reason(),
           static_cast<unsigned>(pending_), clock_ok() ? "set" : "not set");
  net_ = Net::Closed;
  net_until_ms_ = 0;
  net_was_up_ = false;
  // The retry clock starts from the close, not from the boot: that is the duty
  // cycle's down half.
  next_net_retry_ms_ = now + kNetRetryMs;
}

const char* Runtime::net_reason() const {
  switch (net_) {
    case Net::Boot: return "boot";
    case Net::Note: return "note saved";
    case Net::Manual: return "asked";
    case Net::Clock: return "clock unset";
    case Net::Retry: return "retry";
    case Net::Closed: break;
  }
  return "closed";
}

void Runtime::sync_now(uint32_t now) {
  open_net(now, Net::Manual);
  retry_waiting_ = false;
  retry_at_ms_ = 0;
}

bool Runtime::setup_window() const { return io_.wifi && io_.wifi->setup_open(); }

// ---------------------------------------------------------------------------
// The pass

void Runtime::tick(uint32_t now, bool wifi_link) {
  if (!ready_) return;
  const uint32_t dt = last_tick_ms_ ? now - last_tick_ms_ : 0;
  last_tick_ms_ = now;
  if (net_open(now)) radio_ms_ += dt;
  if (wifi_link) net_was_up_ = true;

  // The phone first: a transfer is driven by the commands the link queued.
  step_ble(now);

  if (mode_ == Mode::Recording) {
    if (capture_.full()) finish_note(now, "cap");
    return;
  }
  if (mode_ == Mode::Syncing) {
    step_sync(now);
  }

  if (static_cast<int32_t>(now - next_scan_ms_) >= 0) {
    next_scan_ms_ = now + ((pending_ == 0 && failed_ == 0) ? kIdleScanMs : kScanMs);
    refresh_counts();
  }
  if (static_cast<int32_t>(now - next_battery_ms_) >= 0) {
    next_battery_ms_ = now + kBatteryMs;
    refresh_battery();
  }
  if (static_cast<int32_t>(now - next_retention_ms_) >= 0) run_retention(now, false);

  if (mode_ == Mode::Idle) begin_sync(now);

  // A transfer in flight keeps its window: closing the station under the
  // uploader would waste the bytes it has already sent (`step_sync` above may
  // have left the mode Syncing).
  if (mode_ == Mode::Syncing && net_ != Net::Closed) {
    const uint32_t floor = now + 30000;
    if (static_cast<int32_t>(floor - net_until_ms_) > 0) net_until_ms_ = floor;
  }
  // The window closes as soon as there is nothing left for it to do, or when
  // its deadline passes; the retry clock then brings it back if work remains.
  if (net_ != Net::Closed && !setup_window()) {
    const bool done = net_was_up_ && clock_ok() && pending_ == 0 && mode_ == Mode::Idle &&
                      !ble_sync_.progress().active;
    if (done || static_cast<int32_t>(now - net_until_ms_) >= 0) close_net(now);
  }
  if (net_ == Net::Closed && static_cast<int32_t>(now - next_net_retry_ms_) >= 0 &&
      (pending_ > 0 || !clock_ok())) {
    open_net(now, Net::Retry);
  }
}

void Runtime::on_mic(const int16_t* samples, size_t count) {
  if (mode_ != Mode::Recording || samples == nullptr || count == 0) return;
  // The writer's task does the card work; without it (no memory for the ring)
  // the app task writes, which is what the file did before the writer existed.
  if (writer_ready_) writer_.push(samples, count);
  else capture_.feed(samples, count);
}

void Runtime::on_clock_synced() {
  if (io_.time) io_.time->on_synced();
  ESP_LOGI(TAG, "clock synced");
}

// ---------------------------------------------------------------------------
// Recording

std::string Runtime::on_action(int action) {
  const auto a = static_cast<hgp::notesui::Action>(action);
  const uint32_t now = now_ms();
  switch (a) {
    case hgp::notesui::Action::StartRecording:
      start_note(now);
      return mode_ == Mode::Recording ? "recording " + meta_.note_id : std::string();
    case hgp::notesui::Action::StopKeep: {
      if (mode_ != Mode::Recording) return {};
      const std::string id = meta_.note_id;
      finish_note(now, "button");
      return stop_error_.empty() ? "stored " + id : std::string();
    }
    case hgp::notesui::Action::Discard:
      if (mode_ != Mode::Recording) return {};
      abort_note("button hold");
      return "dropped";
    case hgp::notesui::Action::PowerOff:
      shutdown();
      return "power off";
    default:
      return {};
  }
}

void Runtime::start_note(uint32_t now) {
  if (mode_ != Mode::Idle) return;
  stop_error_.clear();
  if (!io_.mic) {
    error_ = "No microphone";
    ESP_LOGW(TAG, "not recording: this board brought up no microphone");
    return;
  }
  if (!ensure_sd()) {
    error_ = sd_error_.empty() ? "No SD card" : sd_error_;
    return;
  }
  run_retention(now, true);
  if (low_space_) {
    error_ = kLowSpaceError;
    return;
  }

  // The id is resolved against the card: a reboot inside the same second must
  // not reuse a folder.
  const std::string seq = io_.config->get(kSeqKey).value_or("0");
  uint32_t counter = static_cast<uint32_t>(std::strtoul(seq.c_str(), nullptr, 10));
  notes::Meta meta;
  bool created = false;
  std::string error;
  for (int tries = 0; tries < 5 && !created; ++tries) {
    ++counter;
    meta.note_id = notes::make_note_id(device_id_, std::time(nullptr), counter);
    created = capture_.start(store_, meta.note_id, io_.config->note_max_s(), error);
  }
  if (!created) {
    error_ = error.empty() ? "Cannot write to the card" : error;
    sd_ready_ = false;
    sd_mount::release();
    return;
  }
  io_.config->set(kSeqKey, std::to_string(counter));

  meta.device_id = device_id_;
  meta.device_name = device_name_;
  meta.firmware = esp_app_get_description()->version;
  meta.created_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  meta.recorded_at = notes::iso8601_utc(std::time(nullptr));
  meta.idempotency_key = meta.note_id;
  if (battery_mv_) meta.battery_mv = battery_mv_;
  meta_ = std::move(meta);
  writer_.reset_dropped();
  mode_ = Mode::Recording;
  error_.clear();
  io_.mic->start(notes::kSampleRate);
  ESP_LOGI(TAG, "recording %s (cap %d s)", meta_.note_id.c_str(), io_.config->note_max_s());
}

void Runtime::finish_note(uint32_t now, const char* why) {
  if (mode_ != Mode::Recording) return;
  if (io_.mic) io_.mic->stop();
  mode_ = Mode::Idle;
  // The microphone is stopped, so nothing new is pushed: wait for the writer to
  // have the rest on the card before the file is finished.
  if (writer_ready_ && !writer_.drain(30000))
    ESP_LOGW(TAG, "note %s: the card did not take the rest of the audio in 30 s", meta_.note_id.c_str());
  std::string error;
  const bool capped = capture_.full();
  if (!capture_.finish(meta_, error)) {
    ESP_LOGW(TAG, "note not saved (%s): %s", why, error.c_str());
    stop_error_ = error.empty() ? "the recording was not usable" : error;
    error_ = error;
    sd_ready_ = false;
    sd_mount::release();
    refresh_counts();
    return;
  }
  // note.json is the commit: until it lands the note does not exist.
  if (!store_.write_meta(meta_.note_id, meta_, error)) {
    ESP_LOGE(TAG, "note.json not written: %s", error.c_str());
    stop_error_ = "card write failed: " + error;
    store_.discard(notes::NoteState::Pending, meta_.note_id);
    sd_ready_ = false;
    sd_mount::release();
    refresh_counts();
    return;
  }
  error_.clear();
  ESP_LOGI(TAG, "note %s stored: %s, %u s%s", meta_.note_id.c_str(), notes::human_bytes(meta_.bytes).c_str(),
           static_cast<unsigned>(meta_.duration_s), capped ? " (cap reached)" : "");
  // A phone in range can take it in the port's short advertising window; a
  // network window can upload it.
  if (io_.ble) io_.ble->note_saved();
  open_net(now, Net::Note);
  if (const uint32_t lost = writer_.dropped_bytes()) {
    ESP_LOGW(TAG, "note %s lost %u bytes of audio: the card could not keep up", meta_.note_id.c_str(),
             static_cast<unsigned>(lost));
    writer_.discard();
  }
  refresh_counts();
}

void Runtime::abort_note(const char* why) {
  if (io_.mic) io_.mic->stop();
  writer_.discard();
  if (capture_.active()) capture_.abort();
  mode_ = Mode::Idle;
  ESP_LOGW(TAG, "recording dropped: %s", why);
}

// ---------------------------------------------------------------------------
// The queue: one POST at a time, sliced

void Runtime::begin_sync(uint32_t now) {
  if (mode_ != Mode::Idle) return;
  if (!net_open(now) || !clock_ok()) return;
  if (!io_.wifi || io_.wifi->current().empty()) return;  // no association this pass
  if (notes_url().empty()) return;  // no endpoint: nothing to POST to
  if (!ensure_sd()) return;
  if (retry_waiting_ && static_cast<int32_t>(now - retry_at_ms_) < 0) return;
  if (pending_ == 0) return;

  for (const auto& id : store_.pending_ids()) {
    if (id == sync_id_ && retry_waiting_) continue;  // this one is in its backoff
    notes::Meta meta;
    if (!store_.read_meta(id, meta)) continue;
    // A recording that is gone or empty will not come back: the note is
    // terminal, so it moves to failed/ once instead of being rescanned forever.
    const uint64_t audio_bytes = store_.audio_bytes(notes::NoteState::Pending, id);
    if (notes::plan_upload(audio_bytes) != notes::QueueAction::Upload) {
      ESP_LOGW(TAG, "note %s has no recording (%s): moving it to failed/", id.c_str(),
               notes::human_bytes(audio_bytes).c_str());
      store_.move(id, notes::NoteState::Pending, notes::NoteState::Failed);
      refresh_counts();
      continue;
    }
    // A note recorded before the first clock sync carries a 1970 recorded_at.
    // It goes out stamped with the time it is finally sent, written back first
    // so every retry resends the same strings; the id and key never move.
    notes::Meta wire = notes::stamped_for_upload(meta, static_cast<int64_t>(std::time(nullptr)));
    if (wire.recorded_at != meta.recorded_at) {
      ESP_LOGW(TAG, "note %s was recorded at %s, before the clock was set; uploading it as %s", id.c_str(),
               meta.recorded_at.c_str(), wire.recorded_at.c_str());
      std::string error;
      if (!store_.write_meta(id, wire, error))
        ESP_LOGW(TAG, "note %s: cannot record the upload time: %s", id.c_str(), error.c_str());
    }
    std::string error;
    if (!uploader_.begin(wire, store_.audio_path(notes::NoteState::Pending, id), notes_url(), std::time(nullptr),
                         token_, [this](std::string_view canonical) { return sign(canonical); }, error)) {
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
    ESP_LOGI(TAG, "uploading %s (%s)", id.c_str(), notes::human_bytes(meta_.bytes).c_str());
    return;
  }
}

void Runtime::step_sync(uint32_t now) {
  const auto step = uploader_.step();
  if (step == notes::Uploader::Step::Running) return;

  const std::string id = sync_id_;
  mode_ = Mode::Idle;
  sync_id_.clear();
  if (id.empty()) {
    retry_waiting_ = false;
    return;
  }
  if (step == notes::Uploader::Step::Done) {
    if (!store_.move(id, notes::NoteState::Pending, notes::NoteState::Sent))
      ESP_LOGW(TAG, "%s synced but could not be moved to sent/", id.c_str());
    store_.prune_sent();
    attempt_ = 0;
    retry_waiting_ = false;
    error_.clear();
    refresh_counts();
    ESP_LOGI(TAG, "note %s synced (HTTP %d)", id.c_str(), uploader_.http_status());
    return;
  }
  if (step == notes::Uploader::Step::Rejected) {
    ESP_LOGW(TAG, "note %s rejected (HTTP %d): %s", id.c_str(), uploader_.http_status(), uploader_.error().c_str());
    store_.move(id, notes::NoteState::Pending, notes::NoteState::Failed);
    retry_waiting_ = false;
    attempt_ = 0;
    refresh_counts();
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
}

std::string Runtime::sign(std::string_view canonical) const {
  if (key_.size() != 32) return {};
  const auto mac = hg::crypto::hmac_sha256(key_.data(), key_.size(),
                                           reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
  return hg::crypto::base64_encode(mac.data(), mac.size());
}

std::string Runtime::notes_url() const {
  // `notes_url` overrides everything (a host behind a different port or a
  // proxy); otherwise the configured server's host, on the notes port.
  if (io_.config) {
    if (auto stored = io_.config->get(kNotesUrlKey); stored && !stored->empty()) return *stored;
  }
  std::string base = io_.storage->get("server").value_or(std::string(CONFIG_HG_DEFAULT_SERVER_URL));
  const size_t scheme = base.find("://");
  if (scheme == std::string::npos) return {};
  std::string host = base.substr(scheme + 3);
  const size_t slash = host.find('/');
  if (slash != std::string::npos) host.resize(slash);
  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) host.resize(colon);
  if (host.empty()) return {};
  char buf[128];
  std::snprintf(buf, sizeof buf, "http://%s:%u/notes/v1", host.c_str(), static_cast<unsigned>(kDefaultNotesPort));
  return buf;
}

// ---------------------------------------------------------------------------
// The phone's transfer

void Runtime::step_ble(uint32_t) {
  if (!io_.ble || !ble_source_ || !io_.ble->available()) return;
  const uint32_t now = now_ms();
  if (!io_.ble->connected()) {
    if (ble_linked_) {
      ble_linked_ = false;
      // The note stays pending/: the phone resumes from the offset it
      // acknowledged, so a dropped link costs the bytes in flight, nothing more.
      ble_sync_.disconnected();
      refresh_counts();
    }
    return;
  }
  ble_linked_ = true;
  ble_sync_.set_mtu(io_.ble->mtu());

  std::string line;
  while (io_.ble->poll_command(line)) {
    if (!sd_ready_) ensure_sd();
    ESP_LOGI(TAG, "phone: %s", line.c_str());
    ble_sync_.command(line, now);
  }
  ble_sync_.tick(now, [this](const uint8_t* data, size_t len) { return io_.ble->send_frame(data, len); });

  if (ble_sync_.took_delivery()) {
    ESP_LOGI(TAG, "note delivered over Bluetooth");
    retry_waiting_ = false;
    refresh_counts();
  }
}

// ---------------------------------------------------------------------------
// The card

bool Runtime::ensure_sd() {
  if (sd_ready_) return true;
  const uint32_t now = now_ms();
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

void Runtime::refresh_counts() {
  if (!ensure_sd()) return;
  pending_ = store_.count(notes::NoteState::Pending);
  sent_ = store_.count(notes::NoteState::Sent);
  failed_ = store_.count(notes::NoteState::Failed);
  // A waiting note is the reason to advertise: the port opens a window for it.
  if (io_.ble) io_.ble->pending(pending_);
}

void Runtime::run_retention(uint32_t now, bool force) {
  next_retention_ms_ = now + kRetentionMs;
  if (mode_ != Mode::Idle) return;
  if (!ensure_sd()) {
    low_space_ = false;
    next_retention_ms_ = now + kSdRetryMs;
    return;
  }
  notes::CardSpace card;
  uint64_t total = 0, free_bytes = 0;
  if (sd_mount::space(&total, &free_bytes)) {
    card.total = total;
    card.free = free_bytes;
  }
  retention_last_ = notes::enforce(store_, card, static_cast<int64_t>(std::time(nullptr)), retention_);
  if (retention_last_.deleted) {
    ESP_LOGI(TAG, "retention%s: deleted %u synced notes, %s freed", force ? " (asked)" : "",
             static_cast<unsigned>(retention_last_.deleted),
             notes::human_bytes(retention_last_.deleted_bytes).c_str());
    refresh_counts();
  }
  int pct = retention_last_.free_pct_after;
  bool low = retention_last_.low_space;
  uint64_t total_now = 0, free_now = 0;
  if (sd_mount::space(&total_now, &free_now) && total_now) {
    pct = static_cast<int>(free_now * 100ull / total_now);
    low = pct < retention_.min_free_pct;
  }
  retention_last_.free_pct_after = pct;
  retention_last_.low_space = low;
  if (low && !low_space_) ESP_LOGW(TAG, "card %d%% free and no synced notes left to delete", pct);
  low_space_ = low;
  error_ = low ? kLowSpaceError : (error_ == kLowSpaceError ? std::string() : error_);
  if (low) next_retention_ms_ = now + kRetentionLowMs;
}

// ---------------------------------------------------------------------------
// Status and console

Status Runtime::status(uint32_t now) const {
  Status s;
  s.pending = pending_;
  s.sent = sent_;
  s.failed = failed_;
  s.stored = pending_ + sent_ + failed_;
  s.sd_ready = sd_ready_;
  s.clock_set = clock_ok();
  s.battery_pct = battery_pct_;
  s.battery_mv = battery_mv_;
  s.charging = charging_;
  s.external_power = external_;
  s.wifi_window = net_open(now);
  s.wifi = io_.wifi ? io_.wifi->current() : std::string();
  s.error = error_;
  switch (mode_) {
    case Mode::Recording:
      s.phase = Status::Phase::Recording;
      s.recording_s = capture_.seconds();
      break;
    case Mode::Syncing:
      s.phase = Status::Phase::Uploading;
      break;
    case Mode::Idle:
      s.phase = s.error.empty() ? Status::Phase::Idle : Status::Phase::Error;
      break;
  }
  if (ble_sync_.progress().active && s.phase == Status::Phase::Idle) s.phase = Status::Phase::Uploading;
  return s;
}

std::string Runtime::console(std::string_view args) {
  const std::string line = trim(args);
  const size_t sp = line.find(' ');
  const std::string cmd = line.substr(0, sp);
  const std::string rest = sp == std::string::npos ? std::string() : trim(std::string_view(line).substr(sp + 1));
  const uint32_t now = now_ms();

  if (cmd.empty() || cmd == "status") {
    char buf[400];
    std::snprintf(buf, sizeof buf,
                  "@notes stored %u pending %u sent %u failed %u | card %s | %s | mode %s | %u B queued | ble %s | "
                  "net %s | wifi %s | clock %s | battery %d%% (%u mV) | radio %u/%u ms",
                  static_cast<unsigned>(pending_ + sent_ + failed_), static_cast<unsigned>(pending_),
                  static_cast<unsigned>(sent_), static_cast<unsigned>(failed_),
                  sd_ready_ ? "ok" : (sd_error_.empty() ? "down" : sd_error_.c_str()),
                  notes_url().empty() ? "no url" : notes_url().c_str(),
                  mode_ == Mode::Recording ? "recording" : mode_ == Mode::Syncing ? "syncing" : "idle",
                  static_cast<unsigned>(mode_ == Mode::Idle ? store_.pending_bytes() : meta_.bytes),
                  io_.ble && io_.ble->available() ? (io_.ble->connected() ? "link" : "ready") : "off",
                  net_open(now) ? net_reason() : "closed",
                  io_.wifi && !io_.wifi->current().empty() ? io_.wifi->current().c_str() : "none",
                  clock_ok() ? "set" : "1970", battery_pct_, static_cast<unsigned>(battery_mv_),
                  static_cast<unsigned>(radio_ms_), static_cast<unsigned>(now));
    return buf;
  }
  if (cmd == "record" || cmd == "start") {
    if (mode_ == Mode::Recording) return "@ok already recording";
    start_note(now);
    if (mode_ == Mode::Recording) return "@ok recording " + meta_.note_id;
    return "@error " + (error_.empty() ? std::string("cannot record (see log)") : error_);
  }
  if (cmd == "stop") {
    if (mode_ != Mode::Recording) return "@error not recording";
    const std::string id = meta_.note_id;
    finish_note(now, "console");
    if (!stop_error_.empty()) return "@error the note was not saved: " + stop_error_;
    return "@ok stored " + id;
  }
  if (cmd == "sync") {
    sync_now(now);
    if (!clock_ok()) return "@error the clock is not set yet";
    if (notes_url().empty()) return "@error no notes endpoint (set server, or notes url)";
    if (pending_ == 0) return "@ok nothing pending";
    return "@ok window opened, " + std::to_string(pending_) + " pending";
  }
  if (cmd == "url") {
    if (rest.empty()) return "@notes url " + (notes_url().empty() ? std::string("(none)") : notes_url());
    if (!io_.config->set(kNotesUrlKey, rest)) return "@error cannot store the url";
    return "@ok url " + rest;
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
      const uint32_t removed = store_.erase_failed();
      refresh_counts();
      return notes::deleted_failed_answer(removed);
    }
    if (mode_ == Mode::Recording && rest == meta_.note_id) return "@error the note is recording";
    if (!sync_id_.empty() && rest == sync_id_) return "@error the note is uploading";
    notes::NoteState from = notes::NoteState::Pending;
    if (!store_.erase(rest, &from)) return notes::unknown_note_answer(rest);
    refresh_counts();
    return notes::deleted_answer(rest);
  }
  if (cmd == "retention") {
    if (!ensure_sd()) return "@error " + (sd_error_.empty() ? std::string("no card") : sd_error_);
    if (mode_ != Mode::Idle) return "@error busy: a note is recording or uploading";
    run_retention(now, true);
    const auto& r = retention_last_;
    char buf[200];
    std::snprintf(buf, sizeof buf, "@ok retention deleted %u (%u age, %u space), %s freed | free %d%% | sent %u/pending %u/failed %u",
                  static_cast<unsigned>(r.deleted), static_cast<unsigned>(r.deleted_for_age),
                  static_cast<unsigned>(r.deleted_for_space), notes::human_bytes(r.deleted_bytes).c_str(),
                  r.free_pct_after, static_cast<unsigned>(sent_), static_cast<unsigned>(pending_),
                  static_cast<unsigned>(failed_));
    return buf;
  }
  if (cmd == "mount") {
    sd_ready_ = false;
    sd_probe_ms_ = 0;
    return ensure_sd() ? "@ok card mounted" : "@error " + sd_error_;
  }
  if (cmd == "prune") {
    const uint32_t removed = store_.prune_sent();
    refresh_counts();
    return "@ok pruned " + std::to_string(removed);
  }
  if (cmd == "ble") {
    if (!io_.ble || !io_.ble->available()) return "@error this build has no Bluetooth note sync";
    const auto& p = ble_sync_.progress();
    char buf[240];
    if (p.active)
      std::snprintf(buf, sizeof buf, "@notes ble link %s mtu %u transfer %s %u%% (%u/%u B)",
                    io_.ble->connected() ? "up" : "down", static_cast<unsigned>(io_.ble->mtu()), p.id.c_str(),
                    static_cast<unsigned>(p.pct), static_cast<unsigned>(p.sent), static_cast<unsigned>(p.total));
    else
      std::snprintf(buf, sizeof buf, "@notes ble link %s mtu %u idle",
                    io_.ble->connected() ? "up" : "down", static_cast<unsigned>(io_.ble->mtu()));
    return buf;
  }
  if (cmd == "battery") {
    refresh_battery();
    char buf[96];
    std::snprintf(buf, sizeof buf, "@ok battery %d%% (%u mV), %s%s", battery_pct_, static_cast<unsigned>(battery_mv_),
                  charging_ ? "charging" : "discharging", external_ ? ", external power" : "");
    return buf;
  }
  if (cmd == "wifi") {
    char buf[160];
    std::snprintf(buf, sizeof buf, "@wifi station %s, window %s (deadline in %d s), radio %s",
                  io_.wifi && !io_.wifi->current().empty() ? io_.wifi->current().c_str() : "off",
                  net_open(now) ? net_reason() : "closed",
                  net_open(now) ? static_cast<int>((net_until_ms_ - now) / 1000) : 0,
                  io_.wifi && io_.wifi->radio_running() ? "on" : "off");
    return buf;
  }
  if (cmd == "clock") {
    const time_t now_s = std::time(nullptr);
    char stamp[32] = "1970-01-01T00:00:00Z";
    const std::string iso = notes::iso8601_utc(now_s);
    std::snprintf(stamp, sizeof stamp, "%s", iso.c_str());
    return std::string("@clock ") + (clock_ok() ? "set" : "not set") + ", " + stamp;
  }
  if (cmd == "poweroff" || cmd == "off") {
    // The same path PWR's hold takes, so a host can prove the flush and the
    // latch release without a finger: it does not return on battery.
    shutdown();
    return ready_ ? "@ok still on USB: card remounted" : "@ok power off";
  }
  return "@error usage: notes [status | record | stop | sync | url | list | delete | retention | mount | prune | ble | battery | wifi | clock | poweroff]";
}

}  // namespace hgn
