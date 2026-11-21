// Routes the notes device's two keys to what they do, moves the mode ticks, and
// returns the glass to the resting face after a quiet minute.
//
//   PWR   a short press talks to Hermes (start a turn, or end the one in
//         progress). A hold is still the latch's own power-off: LatchPower reads
//         the same pin, so a press on its way to switching the board off acts
//         nothing here.
//   BOOT  a tap starts or stops a note; held past 2 s it cancels (the recording
//         is dropped, or a live turn / overlay that took the screen is closed).
//         BOOT never talks.
//   either while a phone asks to pair: BOOT allows, PWR refuses; twenty seconds
//         of neither refuses (the core's kPhoneAskMs).
//
// The key decision itself is HomeKeys (modes/home_keys.hpp) and the idle clock is
// AutoHome (modes/auto_home.hpp); both are hardware-free and host-tested, and this
// class is only the GPIO polling and the calls into the app around them.
// (docs/firmware-modules.md, docs/waveshare-epaper-154-v2.md §6.1)
#pragma once

#include <atomic>
#include <cstdint>

#include "modes/auto_home.hpp"
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
  // outlive the router; app is the one-thing-owns-the-UI entry point.
  void begin(int power_gpio, int record_gpio, hg::App* app, LiveMode* live, NotesMode* notes,
             const HomeKeys::Config& keys);
  // Poll both keys, tick every mode, and go home after a quiet minute; call from
  // the app loop.
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
  // The idle clock's view of the device: what is on the glass, and what is in
  // flight. An ask, a turn, a prompt and an upload all count as interaction.
  AutoHome::Inputs auto_home_inputs() const;
  static void on_key_edge(void* arg);

  int power_gpio_ = -1;
  int record_gpio_ = -1;
  HomeKeys keys_{};
  AutoHome auto_home_{};

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
