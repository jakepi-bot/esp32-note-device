// Durable device configuration for the mode modules: NVS-backed through the
// same hg::Storage the core's settings use (namespace "hgadget"), so values
// survive reboots and show up in `diag`. Keys are short (NVS limits them to
// 14 characters); the mode modules add their own through set()/get().
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "hg/hal.hpp"

namespace hgp {

class DeviceConfig {
 public:
  void begin(hg::Storage& storage);

  // Longest single voice note in seconds; the offline mode caps recordings
  // at it (300 = five minutes, the spec's default).
  int note_max_s() const;
  bool set_note_max_s(int seconds);

  // Boots since the first start: a plain counter that proves NVS writes
  // persist across power cycles (it appears in the boot log).
  uint32_t boot_count() const;

  // Typed access for the mode modules; the same key-length and commit rules
  // as the core's settings.
  std::optional<std::string> get(std::string_view key) const;
  bool set(std::string_view key, std::string_view value);

 private:
  hg::Storage* storage_ = nullptr;
};

}  // namespace hgp
