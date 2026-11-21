// Active-low push buttons, polled from the app task with a short debounce.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "driver/gpio.h"
#include "esp_timer.h"
#include "hg/app.hpp"

namespace hgp {
namespace {
constexpr uint8_t kDebounceTicks = 3;  // 3 polls at ~10 ms
}

void Buttons::begin(const ButtonConfig& cfg) {
  const int gpios[4] = {cfg.talk, cfg.cancel, cfg.up, cfg.down};
  const hg::Button ids[4] = {hg::Button::Talk, hg::Button::Cancel, hg::Button::Up, hg::Button::Down};
  for (int i = 0; i < 4; ++i) {
    buttons_[i].gpio = gpios[i];
    buttons_[i].id = ids[i];
    if (gpios[i] < 0) continue;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gpios[i];
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
  }
  hold_.begin(cfg.talk_hold_cancel_ms);
}

void Buttons::poll(hg::App& app) {
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  for (auto& b : buttons_) {
    if (b.gpio < 0) continue;
    bool down = gpio_get_level(static_cast<gpio_num_t>(b.gpio)) == 0;
    if (down == b.pressed) {
      b.stable = 0;
      // A talk hold past the board's threshold taps CANCEL once, while the
      // key is still down (spec 6.1). Same press-and-release the console's
      // `cancel` command sends, so the core sees one ordinary CANCEL tap.
      if (b.id == hg::Button::Talk && b.pressed && hold_.fired(now)) {
        app.on_button(hg::Button::Cancel, true);
        app.on_button(hg::Button::Cancel, false);
      }
      continue;
    }
    if (++b.stable >= kDebounceTicks) {
      const bool was_down = b.pressed;
      b.pressed = down;
      b.stable = 0;
      if (b.id == hg::Button::Talk) {
        // Armed from the state the press found, before the core reacts to it:
        // a press that starts a fresh recording is talk for as long as it lasts.
        if (down && !was_down) hold_.press(now, hold_cancel_armed(app.in_turn(), app.screen()));
        else if (!down) hold_.release();
      }
      app.on_button(b.id, down);
    }
  }
}

}  // namespace hgp
