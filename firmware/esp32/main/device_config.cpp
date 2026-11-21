// See device_config.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "device_config.hpp"

#include <cstdlib>

#include "esp_log.h"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.config";
constexpr const char* kNoteMaxKey = "note_max_s";
constexpr const char* kBootCountKey = "boot_count";
constexpr int kDefaultNoteMaxS = 300;
constexpr int kMinNoteMaxS = 10;
constexpr int kMaxNoteMaxS = 3600;

int to_int(const std::optional<std::string>& raw, int fallback) {
  if (!raw || raw->empty()) return fallback;
  char* end = nullptr;
  const long value = std::strtol(raw->c_str(), &end, 10);
  if (end == raw->c_str() || *end != '\0') return fallback;
  return static_cast<int>(value);
}

}  // namespace

void DeviceConfig::begin(hg::Storage& storage) {
  storage_ = &storage;
  // The boot counter is this store's write test: if NVS stops working, the
  // number stops growing and the log shows it.
  const uint32_t boots = static_cast<uint32_t>(to_int(storage_->get(kBootCountKey), 0)) + 1;
  storage_->set(kBootCountKey, std::to_string(boots));
  ESP_LOGI(TAG, "durable config ready: boot %u, note_max_s %d", static_cast<unsigned>(boots),
           note_max_s());
}

int DeviceConfig::note_max_s() const {
  const int value = to_int(storage_ ? storage_->get(kNoteMaxKey) : std::nullopt, kDefaultNoteMaxS);
  if (value < kMinNoteMaxS || value > kMaxNoteMaxS) return kDefaultNoteMaxS;
  return value;
}

bool DeviceConfig::set_note_max_s(int seconds) {
  if (seconds < kMinNoteMaxS || seconds > kMaxNoteMaxS) {
    ESP_LOGW(TAG, "note_max_s %d out of range %d..%d", seconds, kMinNoteMaxS, kMaxNoteMaxS);
    return false;
  }
  return set(kNoteMaxKey, std::to_string(seconds));
}

uint32_t DeviceConfig::boot_count() const {
  return static_cast<uint32_t>(storage_ ? to_int(storage_->get(kBootCountKey), 0) : 0);
}

std::optional<std::string> DeviceConfig::get(std::string_view key) const {
  return storage_ ? storage_->get(key) : std::nullopt;
}

bool DeviceConfig::set(std::string_view key, std::string_view value) {
  if (!storage_) return false;
  const bool ok = storage_->set(key, value);
  if (!ok) ESP_LOGW(TAG, "could not save %.*s", static_cast<int>(key.size()), key.data());
  return ok;
}

}  // namespace hgp
