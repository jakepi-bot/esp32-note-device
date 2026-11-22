// When the Bluetooth controller may run at all.
//
// This chip has no 32.768 kHz crystal, so ESP-IDF's controller falls back to
// the main crystal as its sleep clock and the chip cannot light-sleep while
// the controller is enabled (the boot log says so: `BLE_INIT: light sleep mode
// will not be able to apply when bluetooth is enabled`). The radio therefore
// has to be *off* at rest, and a phone can only reach the device inside a
// bounded window: the two-minute pairing window, the few minutes after a note
// is recorded, a periodic retry while notes are still waiting for a phone, and
// an explicit wake. A link that is up always keeps it warm.
//
// Pure policy, no hardware: port_ble.cpp owns the controller and asks this for
// `warm()`, the host tests drive the same object with a fake clock.
#pragma once

#include <cstdint>

namespace hg {

class RadioWindow {
 public:
  // How long each reason keeps the radio up. Bounded on purpose: every minute
  // the controller runs is a minute the board cannot sleep.
  struct Timing {
    uint32_t boot_ms = 120000;       // after boot: let a phone find a new device
    uint32_t pairing_ms = 120000;    // `phone pair` (docs: two minutes)
    uint32_t post_note_ms = 180000;  // a note just landed: a phone may take it
    uint32_t retry_ms = 300000;      // notes still waiting: try this often
    uint32_t retry_hold_ms = 30000;  // how long each retry listens
    uint32_t trigger_ms = 60000;     // an explicit wake (console `phone wake`)
    uint32_t phone_ms = 300000;      // Menu -> Phone: five minutes to pair or sync
    uint32_t phone_link_ms = 60000;  // a connected phone holds the deadline this far out
    uint32_t grace_ms = 20000;       // stay up this long after a window closes
  };

  // Why the radio is up, for the console and the log line.
  enum class Reason : uint8_t { Rest, Boot, Pairing, Note, Pending, Link, Wake, Phone };

  RadioWindow() = default;
  explicit RadioWindow(const Timing& timing) : timing_(timing) {}

  // The device just booted: a window so a phone that is looking finds it.
  void boot(uint32_t now_ms);
  // `phone pair` opened the pairing window (open_pairing/close_pairing in the
  // port own the exchange; this only keeps the controller alive for it).
  void pair(uint32_t now_ms);
  // A note finished recording and sits in pending/.
  void note_saved(uint32_t now_ms);
  // Someone asked for the radio by hand.
  void trigger(uint32_t now_ms);
  // Menu -> Phone: the five-minute window a person asked for, so a phone has
  // time to pair and take the notes waiting on the card.
  void phone(uint32_t now_ms);
  // The phone window was closed by the person (the screen's back): no deadline
  // and no grace after it, so the controller can rest on the next pass.
  void phone_closed(uint32_t now_ms);
  // Whether the pairing exchange may go ahead right now: the `phone pair`
  // window, or the Phone screen's own window.
  bool pairing_open(uint32_t now_ms) const {
    return before(now_ms, pair_until_) || before(now_ms, phone_until_);
  }
  // A link went up (keeps the radio warm) or down (a short grace, then rest).
  void connected(bool up, uint32_t now_ms);
  // How many notes are waiting for a phone. A count that stays above zero
  // while no phone answers keeps trying, on `retry_ms`, never forever-on.
  void set_pending(uint32_t count, uint32_t now_ms);

  // Runs the clocks (the retry schedule) and answers whether the controller
  // should be up right now. Call once per app-loop pass.
  bool tick(uint32_t now_ms);

  bool warm(uint32_t now_ms) const { return why(now_ms) != Reason::Rest; }
  Reason why(uint32_t now_ms) const;
  static const char* name(Reason reason);

  // The phone window's own clock: how long it has left, 0 when none is open.
  // The screen's countdown reads this, and it stops at zero rather than
  // counting through the grace the controller stays up for.
  uint32_t phone_left_ms(uint32_t now_ms) const;

  const Timing& timing() const { return timing_; }

 private:
  static bool before(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(deadline_ms - now_ms) > 0;
  }
  // Each reason sets its own deadline and pulls the grace deadline with it, so
  // the radio does not flap at the edge of a window.
  void open(uint32_t& until, uint32_t now_ms, uint32_t duration_ms, Reason reason);
  // The same, for a deadline that is only ever pushed further out: a connected
  // phone holds the phone window open without shortening anybody's timer.
  void extend(uint32_t& until, uint32_t now_ms, uint32_t duration_ms, Reason reason);

  Timing timing_{};
  uint32_t boot_until_ = 0;
  uint32_t pair_until_ = 0;
  uint32_t note_until_ = 0;
  uint32_t trigger_until_ = 0;
  uint32_t phone_until_ = 0;
  bool phone_wanted_ = false;  // the person opened the screen's window and has not closed it
  uint32_t grace_until_ = 0;
  uint32_t retry_until_ = 0;
  uint32_t next_retry_ms_ = 0;
  uint32_t pending_ = 0;
  bool linked_ = false;
  // Which window the grace is the tail of, so `why()` can still name it.
  Reason tail_ = Reason::Rest;
};

}  // namespace hg
