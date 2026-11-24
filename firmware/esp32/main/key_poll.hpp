// Key timing that survives a sleeping chip.
//
// The keys are polled, not interrupt-driven, and the poll also happens after the
// CPU has been in light sleep - where the wall clock keeps running but nobody is
// sampling. A press whose release lands after such a gap was being measured as
// the whole gap: a tap became a hold, and on this device a BOOT hold opens the
// menu, so "press BOOT to record" produced the menu instead (Jake, 2026-11-24).
//
// The rule: time the finger was not observed does not count. When two samples are
// further apart than kMaxGapMs, the slept part is discounted from the press.
#ifndef HGP_KEY_POLL_HPP
#define HGP_KEY_POLL_HPP

#include <cstdint>

namespace hgp {

// The cadence while a key is down: fast enough that a 100 ms tap is three
// samples, and that the release is seen before any hold timer can run.
inline constexpr uint32_t kKeyPollMs = 10;

struct KeyPoll {
  bool down = false;
  bool armed = false;    // seen up since boot / since a power change
  bool ignored = false;  // this press began while presses did not count
  uint32_t since = 0;    // when the press started, in observed time
  uint32_t polled_at = 0;  // the previous sample's clock
  uint32_t slept_at = 0;   // light sleep counted so far, to discount new sleep
};

enum class KeyEdge { None, Tap, Hold };

// Fold one sample into the poll. `lost_ms` is the part of the gap that the chip
// was asleep for (0 when the caller knows it was awake the whole time).
inline KeyEdge feed(KeyPoll& k, bool pressed, uint32_t now, uint32_t hold_ms, uint32_t lost_ms = 0) {
  if (!pressed) {
    k.armed = true;
    if (!k.down) return KeyEdge::None;
    k.down = false;
    if (k.ignored) {  // the tail of a press that never counted
      k.ignored = false;
      return KeyEdge::None;
    }
    return (now - k.since >= hold_ms) ? KeyEdge::Hold : KeyEdge::Tap;
  }
  if (k.down) return KeyEdge::None;
  k.down = true;
  k.since = now;
  return KeyEdge::None;
}

// While a key is down, only the time the chip actually slept is discounted: a
// busy loop that took 200 ms to answer is still a finger on the key, and an
// upload chunk is exactly that long (2026-11-24). The caller passes the light
// sleep it measured, not an inference from the gap.
inline void discount_sleep(KeyPoll& k, uint32_t slept_ms) {
  if (!k.down || slept_ms == 0) return;
  k.since += slept_ms;
}

}  // namespace hgp

#endif  // HGP_KEY_POLL_HPP
