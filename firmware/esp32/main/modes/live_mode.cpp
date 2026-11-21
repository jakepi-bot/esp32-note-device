// See live_mode.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/live_mode.hpp"

#include <string>

#include "esp_log.h"
#include "hg/app.hpp"
#include "ui_state.hpp"

namespace hgp {
namespace {
constexpr const char* TAG = "hg.live";
}

void LiveModeImpl::begin(hg::App& app, UiState& ui) {
  app_ = &app;
  ui_ = &ui;
  ESP_LOGI(TAG, "live mode: BOOT records and streams; the notice line follows the turn");
}

void LiveModeImpl::tick(uint32_t) {
  if (!app_ || !ui_) return;
  state_.update(app_->screen(), ui_->phase());
  apply();
}

void LiveModeImpl::set_connected(bool connected, std::string_view why) {
  if (!app_ || !ui_) return;
  state_.transport(connected, app_->in_turn(), ui_->phase(), std::string(why));
  apply();
}

void LiveModeImpl::apply() {
  if (state_.should_clear_error()) ui_->clear_error();
  if (!state_.should_push()) return;
  if (state_.phase() == UiPhase::Error) ESP_LOGW(TAG, "%s", state_.detail().c_str());
  ui_->set(state_.phase(), state_.detail());
}

}  // namespace hgp
