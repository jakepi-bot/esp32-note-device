// See mode_router.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/mode_router.hpp"

#include "driver/gpio.h"
#include "modes/live_mode.hpp"
#include "modes/notes_mode.hpp"

namespace hgp {
namespace {

// A tap is a press shorter than this. LatchPower releases the battery latch
// after its own, longer hold (2.5 s), so a press that lasts this long is on
// its way to powering off and must not also start a note.
constexpr uint32_t kMaxTapMs = 1500;

}  // namespace

void ModeRouter::begin(int note_gpio, LiveMode* live, NotesMode* notes) {
  note_gpio_ = note_gpio;
  live_ = live;
  notes_ = notes;
  if (note_gpio_ >= 0) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << note_gpio_;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);  // LatchPower configures the same pin the same way
  }
}

void ModeRouter::tick(uint32_t now_ms) {
  if (note_gpio_ >= 0) {
    const bool down = gpio_get_level(static_cast<gpio_num_t>(note_gpio_)) == 0;
    if (down && !down_) {
      down_ = true;
      down_at_ms_ = now_ms;
    } else if (!down && down_) {
      down_ = false;
      if (now_ms - down_at_ms_ <= kMaxTapMs && notes_) notes_->on_note_button();
    }
  }
  if (live_) live_->tick(now_ms);
  if (notes_) notes_->tick(now_ms);
}

}  // namespace hgp
