// See ui_state.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "ui_state.hpp"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "hg/app.hpp"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.ui";
// How long each published line stays on screen without a republish. The
// Recording line re-publishes every second from tick(); the others are set
// once per phase and expire on their own.
constexpr uint32_t kRecordingTtlMs = 2000;
constexpr uint32_t kUploadingTtlMs = 4000;
constexpr uint32_t kErrorTtlMs = 10000;
constexpr uint32_t kStatusTtlMs = 60000;
constexpr uint32_t kStatusRefreshMs = 45000;
constexpr uint32_t kRecordTickMs = 1000;
// How often keep_alive() may republish a line that outlives its ttl; shorter
// than every ttl above, long enough that an e-paper panel is not repainted.
constexpr uint32_t kKeepAliveMs = 4000;

const char* phase_name(UiPhase phase) {
  switch (phase) {
    case UiPhase::Idle: return "idle";
    case UiPhase::Recording: return "recording";
    case UiPhase::Uploading: return "uploading";
    case UiPhase::Error: return "error";
  }
  return "?";
}

}  // namespace

void UiState::bind(hg::App& app) { app_ = &app; }

void UiState::set(UiPhase phase, std::string detail) {
  if (phase == UiPhase::Error && phase_ != UiPhase::Error && detail.empty()) detail = "error";
  if (phase == phase_) {
    if (!detail.empty()) detail_ = std::move(detail);
    return;
  }
  if (phase_ == UiPhase::Error && phase != UiPhase::Idle) {
    ESP_LOGW(TAG, "ignored %s -> %s: an error clears through idle", phase_name(phase_),
             phase_name(phase));
    return;
  }
  const UiPhase from = phase_;
  phase_ = phase;
  detail_ = std::move(detail);
  entered_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  last_publish_ms_ = 0;
  ESP_LOGI(TAG, "%s -> %s%s%s", phase_name(from), phase_name(phase), detail_.empty() ? "" : ": ",
           detail_.c_str());
  publish(entered_ms_);
  if (on_change) on_change(from, phase_, detail_);
}

void UiState::clear_error() {
  if (phase_ == UiPhase::Error) set(UiPhase::Idle);
}

void UiState::set_status(std::string text) {
  if (text == status_) return;
  status_ = std::move(text);
  if (phase_ == UiPhase::Idle) publish(static_cast<uint32_t>(esp_timer_get_time() / 1000));
}

void UiState::tick(uint32_t now_ms) {
  if (phase_ == UiPhase::Idle) {
    // Keep the mode's status alive on a panel that never repaints by itself.
    if (!status_.empty() && now_ms - last_publish_ms_ >= kStatusRefreshMs) publish(now_ms);
    return;
  }
  if (phase_ != UiPhase::Recording || now_ms - last_publish_ms_ < kRecordTickMs) return;
  publish(now_ms);
}

void UiState::keep_alive(uint32_t now_ms) {
  if (phase_ == UiPhase::Idle) return;
  // A republish every few seconds is enough to stay ahead of every ttl above
  // (the shortest is the recording line's 2 s, which tick() already keeps up).
  if (last_publish_ms_ != 0 && now_ms - last_publish_ms_ < kKeepAliveMs) return;
  publish(now_ms);
}

void UiState::publish(uint32_t now_ms) {
  last_publish_ms_ = now_ms;
  if (!app_) return;
  std::string text;
  uint32_t ttl = 0;
  switch (phase_) {
    case UiPhase::Idle:
      // The mode's own line if it has one, otherwise nothing: the previous
      // line expires by its ttl.
      text = status_;
      ttl = kStatusTtlMs;
      break;
    case UiPhase::Recording: {
      const uint32_t seconds = (now_ms - entered_ms_) / 1000;
      char buf[32];
      std::snprintf(buf, sizeof(buf), "Recording %02u:%02u", static_cast<unsigned>(seconds / 60),
                    static_cast<unsigned>(seconds % 60));
      text = buf;
      ttl = kRecordingTtlMs;
      break;
    }
    case UiPhase::Uploading:
      text = detail_.empty() ? std::string("Uploading") : "Uploading " + detail_;
      ttl = kUploadingTtlMs;
      break;
    case UiPhase::Error:
      text = detail_;
      ttl = kErrorTtlMs;
      break;
  }
  if (!text.empty() || phase_ == UiPhase::Idle) app_->local_notice(std::move(text), ttl);
}

}  // namespace hgp
