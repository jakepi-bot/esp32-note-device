// Routes the buttons that belong to the mode modules to them: the PWR key
// (a tap starts a voice note; holding it still powers the board off through
// LatchPower) and the app loop's tick fan-out. The TALK key stays with the
// core, which records and streams it itself (docs/firmware-modules.md).
#pragma once

#include <cstdint>

namespace hgp {

class LiveMode;
class NotesMode;

class ModeRouter {
 public:
  // note_gpio is the PWR key (-1 when the board has none); live and notes
  // are borrowed and outlive the router.
  void begin(int note_gpio, LiveMode* live, NotesMode* notes);
  // Poll the note key and tick both modes; call from the app loop.
  void tick(uint32_t now_ms);

 private:
  int note_gpio_ = -1;
  bool down_ = false;
  uint32_t down_at_ms_ = 0;
  LiveMode* live_ = nullptr;
  NotesMode* notes_ = nullptr;
};

}  // namespace hgp
