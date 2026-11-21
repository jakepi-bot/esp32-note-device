// See modes/notes_screen.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/notes_screen.hpp"

#include "esp_log.h"
#include "hg/app.hpp"
#include "port_display_epd.hpp"

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

namespace hgp {
namespace {

constexpr const char* TAG = "hg.status";

// The build's Bluetooth stack, if it has one. This board compiles it out
// (firmware/esp32/platformio.ini, "Wi-Fi only: no BLE on this board"): the icon
// then carries the cut mark and the words never claim a link that cannot exist.
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
constexpr bool kHasBluetooth = true;
#else
constexpr bool kHasBluetooth = false;
#endif

// The screens this one replaces: the device is resting, and none of them wants
// a key, a turn or a prompt. Pairing is here because the core UI draws it
// white-on-black, which made the panel flash inverted at the very moment a
// phone was paired (the hardware QA's D3); its words are the core model's own,
// so the screen only changes the paper. Everything else (boot, connecting,
// listening, thinking, responding, the clock face, settings, prompts, errors,
// OTA) stays with the core UI. The one Setup screen this face takes is the
// pairing *ask* (App::on_phone_ask), whose answer the two keys give.
bool rests_on(hg::Screen s) {
  switch (s) {
    case hg::Screen::Ready:
    case hg::Screen::Offline:
    case hg::Screen::LowBattery:
    case hg::Screen::Charging:
    case hg::Screen::Charged:
    case hg::Screen::Pairing: return true;
    default: return false;
  }
}

}  // namespace

void NotesScreen::begin(hg::App& app, NotesMode& notes, hg::System* system, EpdDisplay& display) {
  app_ = &app;
  notes_ = &notes;
  system_ = system;
  display_ = &display;
  ESP_LOGI(TAG, "status screen ready (%s bluetooth)", kHasBluetooth ? "with" : "without");
}

StatusInputs NotesScreen::gather() {
  StatusInputs in;
  if (system_ != nullptr) {
    hg::WallTime wall{};
    in.time_valid = system_->wall_time(wall);
    in.hour = wall.hour;
    in.minute = wall.minute;
  }

  in.link_online = app_->online();
  in.network_up = app_->network_up();
  in.bt_supported = kHasBluetooth;
  in.bt_paired = app_->phones_paired() > 0;
  in.bt_connected = app_->phone_connected();

  // The core's own power read and battery rules: one number on the panel, not a
  // second opinion from here (and no ADC read per frame).
  if (const auto pct = app_->displayed_battery_percent()) in.battery_pct = *pct;
  in.external_power = app_->external_power().value_or(false);

  const NoteStatus n = notes_->status();
  in.sd_ready = n.sd_ready;
  in.sd_free_bytes = n.sd_free_bytes;
  in.sd_free_known = n.sd_free_known;
  in.recording_s = n.recording_s;
  in.pending = n.pending;
  in.synced = n.synced;
  in.failed = n.failed;
  in.stored = n.stored;
  in.uploading_bytes = n.uploading_bytes;
  in.uploading_valid = n.uploading_valid;
  in.uploading_hour = n.uploading_hour;
  in.uploading_minute = n.uploading_minute;
  in.has_newest = n.has_newest;
  in.newest_valid = n.newest_valid;
  in.newest_hour = n.newest_hour;
  in.newest_minute = n.newest_minute;
  in.newest_duration_s = n.newest_duration_s;
  in.error = n.error;
  switch (n.phase) {
    case NoteStatus::Phase::Recording: in.notes = NotesPhase::Recording; break;
    case NoteStatus::Phase::Uploading: in.notes = NotesPhase::Uploading; break;
    case NoteStatus::Phase::Error: in.notes = NotesPhase::Error; break;
    case NoteStatus::Phase::Idle: in.notes = NotesPhase::Idle; break;
  }
  return in;
}

bool NotesScreen::draw(const StatusView& view, bool repaint) {
  if (display_ == nullptr) return false;
  if (repaint) display_->request_full_refresh();
  if (!screen_.draw(*display_, view, repaint)) return false;
  owned_ = true;
  return true;
}

bool NotesScreen::render(const hg::UiModel& model, bool force) {
  if (display_ == nullptr || notes_ == nullptr || app_ == nullptr) return false;
  // A phone asking to pair is the one Setup screen this face takes: the question
  // has to be on the glass that the keys answer it from.
  const bool ask_face = app_->phone_ask_pending() && model.screen == hg::Screen::Setup;
  if (!rests_on(model.screen) && !ask_face) {
    if (owned_) {
      // Back to the core UI: its bands have to be repainted over this image,
      // and the panel's own full-refresh counter is too slow to wait for.
      display_->request_full_refresh();
      owned_ = false;
    }
    return false;
  }

  // Taking the panel, or taking it back: the glass holds another image, so the
  // next update must be a full one (a partial leaves the old screen ghosted).
  const bool repaint = force || !owned_;
  const StatusInputs in = gather();
  const StatusView view =
      ask_face ? compose_ask(in, app_->phone_ask_peer())
      : model.screen == hg::Screen::Pairing
          ? compose_pairing(in, PairingCopy{model.headline, model.detail, model.hint, model.code})
          : compose_status(in);
  return draw(view, repaint);
}

}  // namespace hgp
