// The device-side UI phase the mode modules drive: what the screen should
// say the gadget is doing. Single-threaded like the core: set() is called
// from the task that calls App::tick().
//
// The phase text goes out on the core's notice line through
// App::local_notice(), with a short time-to-live, so a phase that stops
// republishing (Idle) never leaves a stale line behind. Modes that render
// their own band (counts, progress) subscribe with on_change instead.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hg {
class App;
}

namespace hgp {

enum class UiPhase : uint8_t { Idle, Recording, Uploading, Error };

class UiState {
 public:
  // Where phase text is shown. Unbound, transitions only log.
  void bind(hg::App& app);

  // Enter a phase. Any phase may fail into Error; Error leaves only for
  // Idle (use clear_error()). Re-setting the same phase only updates the
  // detail line.
  void set(UiPhase phase, std::string detail = {});
  void clear_error();

  // A line of a mode's own that stays while the mode says so (the offline
  // notes' counts). It shows whenever the phase is Idle, so a phase's line
  // still wins while that phase lasts and the status returns with Idle; it is
  // republished only when it changes or its slow ttl runs low, which keeps an
  // e-paper panel from flashing every second.
  void set_status(std::string text);
  const std::string& status() const { return status_; }

  UiPhase phase() const { return phase_; }
  const std::string& detail() const { return detail_; }
  // Milliseconds spent in the current phase (esp_timer clock).
  uint32_t elapsed_ms(uint32_t now_ms) const { return now_ms - entered_ms_; }

  // Called from the app loop: keeps the Recording line counting while the
  // phase lasts.
  void tick(uint32_t now_ms);

  // Keeps the current phase's line from expiring, for a state that outlasts the
  // notice ttl (the notes mode's low-space error stays while the card stays
  // full). Republishing the same text does not change the model, so the panel
  // does not repaint; only the expiry would have. A no-op while Idle.
  void keep_alive(uint32_t now_ms);

  // Observers for the mode modules (optional).
  std::function<void(UiPhase from, UiPhase to, const std::string& detail)> on_change;

 private:
  void publish(uint32_t now_ms);

  hg::App* app_ = nullptr;
  UiPhase phase_ = UiPhase::Idle;
  std::string detail_;
  std::string status_;
  uint32_t entered_ms_ = 0;
  uint32_t last_publish_ms_ = 0;
};

}  // namespace hgp
