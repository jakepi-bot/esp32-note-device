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

#include <atomic>
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
  // A press is being made or a debounce is running: the loop must not rest.
  // True also when no edge interrupt could be installed, which keeps the
  // original 10 ms polling as the fallback.
  bool fast_poll() const;
  // Re-aim the keys at light sleep. Dozing: the edge interrupt is off and a low
  // level on either key wakes the chip. Awake: the edge interrupt is back.
  void arm_sleep_wake(bool dozing);

 private:
  void act(HomeAction action);
  static void on_key_edge(void* arg);

  int power_gpio_ = -1;
  int record_gpio_ = -1;
  HomeKeys keys_{};

  // The edge interrupt: the loop's resting wait ends at once on a press, and
  // fast_poll() keeps the debounce at its original 10 ms cadence.
  bool edges_armed_ = false;
  std::atomic<uint32_t> edge_ms_{0};

  // Debounce per key: a sampled level has to hold for a few passes.
  bool power_raw_ = false;
  bool record_raw_ = false;
  uint8_t power_stable_ = 0, record_stable_ = 0;

  hg::App* app_ = nullptr;
  LiveMode* live_ = nullptr;
  NotesMode* notes_ = nullptr;
};

}  // namespace hgp
