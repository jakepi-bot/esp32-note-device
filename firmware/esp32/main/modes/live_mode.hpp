// Live mode: the device-side state around the core's voice pipeline. The
// core records, streams over the WebSocket and plays the reply — BOOT reaches
// App::on_button — and this module owns what the notice line says while that
// happens and what a lost connection does to it.
//
// LiveState (modes/live_state.hpp) holds the decisions, hardware-free so the
// host tests drive them; LiveModeImpl is the ESP32 wiring over App and
// UiState. See docs/firmware-modules.md.
#pragma once

#include <cstdint>
#include <string_view>

#include "modes/live_state.hpp"

namespace hg {
class App;
}

namespace hgp {

class LiveMode {
 public:
  virtual ~LiveMode() = default;
  // Called once after the core has started, with the services the mode
  // shows its state through.
  virtual void begin(hg::App& app, UiState& ui) = 0;
  // The app loop's heartbeat (every pass, esp_timer milliseconds).
  virtual void tick(uint32_t now_ms) = 0;
  // The Hermes transport opened or closed; the mode answers with its own
  // status and error states, never by blocking the loop. why is the close
  // reason (empty when the transport opened).
  virtual void set_connected(bool connected, std::string_view why) = 0;
};

// Drives UiState from LiveState: Recording while the microphone is open,
// Uploading while the turn waits for Hermes, an Error line with the close
// reason when a connection that was up drops mid-turn, cleared on reconnect.
class LiveModeImpl final : public LiveMode {
 public:
  void begin(hg::App& app, UiState& ui) override;
  void tick(uint32_t now_ms) override;
  void set_connected(bool connected, std::string_view why) override;

 private:
  void apply();
  hg::App* app_ = nullptr;
  UiState* ui_ = nullptr;
  LiveState state_;
};

}  // namespace hgp
