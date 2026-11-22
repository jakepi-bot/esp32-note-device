// The battery's percentage, one conversion for the console and the glass.
//
// Two readers ask the same port the same question - Runtime, which prints it in
// `notes status` and `battery`, and NotesUi, which draws the band's level - and
// they must not answer differently: the same rule docs/notes-ui-spec.md §3.2
// already holds `radio`/`ble` to. It used to be two answers: LatchPower fills
// only battery_mv on this board, the runtime converted the mV, and the UI copied
// battery_percent - which no board sets - so the band's fact was -1 forever
// while the console printed 90 %.
#pragma once

#include <cstdint>

#include "battery_curve.hpp"
#include "hg/hal.hpp"

namespace hgp {

// The linear fallback for a board with no measured discharge curve. These are
// the numbers the console has always printed (Runtime's kEmptyMv..kFullMv);
// they stay here so moving the conversion cannot move the answer.
constexpr uint16_t kBatteryEmptyMv = 3300, kBatteryFullMv = 4150;

// A port that answers a percent wins; else the board's own curve; else the
// linear fallback. -1 only when the port answered nothing about the battery at
// all: Power::read's contract says a failed read is not a reading, so it must
// never become a level (the band draws '?' for -1, never a full bar).
inline int battery_percent(const hg::PowerStatus& status, hg::Curve curve) {
  if (status.battery_percent) return *status.battery_percent;
  if (!status.battery_mv) return -1;
  const uint16_t mv = *status.battery_mv;
  if (curve) return hg::percent_on_curve(curve, mv);
  if (mv <= kBatteryEmptyMv) return 0;
  if (mv >= kBatteryFullMv) return 100;
  return static_cast<int>((mv - kBatteryEmptyMv) * 100u / (kBatteryFullMv - kBatteryEmptyMv));
}

}  // namespace hgp
