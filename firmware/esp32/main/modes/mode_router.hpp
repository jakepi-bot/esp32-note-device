// Routes the notes device's two keys to what they do, and fans the app loop's
// tick out to the mode modules.
//
//   PWR   a tap goes home (App::go_home), whatever is open. A hold is still the
//         latch's own power-off: LatchPower reads the same pin, so a press on its
//         way to switching the board off acts nothing here.
//   BOOT  a tap starts or stops a note; held, it talks, so the plain SKU (no touch
//         controller on the glass) can still reach the live mode. On a Touch SKU
//         the home screen's TALK button does that and a hold acts nothing.
//
// The decision itself is HomeKeys (modes/home_keys.hpp), which is hardware-free
// and host-tested; this class is only the GPIO polling around it.
// (docs/firmware-modules.md, docs/waveshare-epaper-154-v2.md §6.1)
#pragma once

#include <cstdint>

#include "modes/home_keys.hpp"

namespace hg {
class App;
}

namespace hgp {

class LiveMode;
class NotesMode;

class ModeRouter {
 public:
  // -1 for a key the board does not have. live and notes are borrowed and
  // outlive the router; app is the one-thing-owns-the-UI entry point (go_home).
  void begin(int power_gpio, int record_gpio, hg::App* app, LiveMode* live, NotesMode* notes,
             const HomeKeys::Config& keys);
  // Poll both keys and tick every mode; call from the app loop.
  void tick(uint32_t now_ms);

 private:
  void act(HomeAction action);

  int power_gpio_ = -1;
  int record_gpio_ = -1;
  HomeKeys keys_{};

  // Debounce per key: a sampled level has to hold for a few passes.
  bool power_raw_ = false;
  bool record_raw_ = false;
  uint8_t power_stable_ = 0, record_stable_ = 0;

  hg::App* app_ = nullptr;
  LiveMode* live_ = nullptr;
  NotesMode* notes_ = nullptr;
};

}  // namespace hgp
