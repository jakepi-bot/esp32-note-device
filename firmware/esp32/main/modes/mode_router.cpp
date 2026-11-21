// See modes/mode_router.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "modes/mode_router.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "hg/app.hpp"
#include "modes/live_mode.hpp"
#include "modes/notes_mode.hpp"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.keys";
// 3 polls at ~10 ms, the debounce port_buttons.cpp uses for the same kind of key.
constexpr uint8_t kDebounceTicks = 3;
// After an edge the loop polls at 10 ms for this long: long enough for the
// 3-tick debounce and for a hold to start acting without a visible lag.
constexpr uint32_t kFastWindowMs = 400;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// One key press as the core's own button: the menu navigates with CANCEL and
// TALK, so the two physical keys borrow those names rather than a new grammar.
void press(hg::App* app, hg::Button button) {
  app->on_button(button, true);
  app->on_button(button, false);
}

}  // namespace

void ModeRouter::begin(int power_gpio, int record_gpio, hg::App* app, LiveMode* live, NotesMode* notes,
                       const HomeKeys::Config& keys) {
  power_gpio_ = power_gpio;
  record_gpio_ = record_gpio;
  app_ = app;
  live_ = live;
  notes_ = notes;
  keys_.begin(keys);
  const int gpios[2] = {power_gpio_, record_gpio_};
  for (int gpio : gpios) {
    if (gpio < 0) continue;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gpio;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    // Both edges, so a press wakes the app loop out of its resting wait and a
    // release does the same. The handler only records the time and posts a
    // wake-up; the level poll below still decides what the key did.
    io.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io);  // LatchPower configures the power pin the same way
    // Hold the pull-up and the level through light sleep, so the key's own
    // press is what wakes the chip (arm_sleep_wake()).
    gpio_sleep_sel_dis(static_cast<gpio_num_t>(gpio));
    if (hgp::gpio_isr_service() &&
        gpio_isr_handler_add(static_cast<gpio_num_t>(gpio), &ModeRouter::on_key_edge, this) == ESP_OK) {
      edges_armed_ = true;
    } else {
      ESP_LOGW(TAG, "key GPIO %d has no edge interrupt; polling it", gpio);
    }
  }
  ESP_LOGI(TAG, "keys: power %d (talk), record %d (note; held: menu or cancel), auto-home %u s", power_gpio_,
           record_gpio_, static_cast<unsigned>(kAutoHomeSeconds));
}

// GPIO ISR: no allocation and nothing that blocks, only a timestamp and an
// ISR-safe queue send that wakes the app loop.
void ModeRouter::on_key_edge(void* arg) {
  auto* self = static_cast<ModeRouter*>(arg);
  self->edge_ms_.store(now_ms(), std::memory_order_relaxed);
  hgp::events::post_from_isr(EventType::KeyEdge);
}

bool ModeRouter::fast_poll() const {
  // No interrupt was installed: never slow the loop down, or a press could wait
  // a quarter second before its first sample.
  if (!edges_armed_) return true;
  if (keys_.key_down() || keys_.power_down()) return true;
  const uint32_t edge = edge_ms_.load(std::memory_order_relaxed);
  if (edge != 0 && now_ms() - edge < kFastWindowMs) return true;
  // A level that changed but has not been debounced in yet.
  const int gpios[2] = {power_gpio_, record_gpio_};
  for (int gpio : gpios) {
    if (gpio >= 0 && gpio_get_level(static_cast<gpio_num_t>(gpio)) == 0) return true;
  }
  return false;
}

void ModeRouter::arm_sleep_wake(bool dozing) {
  const int gpios[2] = {power_gpio_, record_gpio_};
  for (int gpio : gpios) {
    if (gpio < 0) continue;
    const auto pin = static_cast<gpio_num_t>(gpio);
    if (dozing) {
      // A level interrupt and the edge handler cannot share the pin's type
      // register, so the edge handler is off while light sleep runs. The key
      // conducts to ground: a low level is a press.
      if (edges_armed_) gpio_intr_disable(pin);
      gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
    } else {
      gpio_wakeup_disable(pin);
      if (edges_armed_) {
        gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
        gpio_intr_enable(pin);
      }
    }
  }
  if (dozing) esp_sleep_enable_gpio_wakeup();
}

void ModeRouter::act(HomeAction action) {
  if (action == HomeAction::None || app_ == nullptr) return;
  // What is on the glass decides what a held key means: the pairing question
  // owns both keys, the menu takes taps as next/change, and only a quiet home
  // screen turns a held BOOT into "open the menu" (modes/home_keys.hpp).
  KeyContext context;
  context.phone_ask = app_->phone_ask_pending();
  context.menu_open = app_->settings_open();
  const hg::Screen screen = app_->model().screen;
  context.turn_or_overlay = app_->in_turn() || !auto_home_face(screen);
  if (notes_ != nullptr) context.note_recording = notes_->status().phase == NoteStatus::Phase::Recording;

  switch (route_key(action, context)) {
    case KeyRoute::None: break;
    case KeyRoute::AskAllow:
      ESP_LOGI(TAG, "pairing ask: BOOT allows");
      app_->answer_phone_pairing(true);
      break;
    case KeyRoute::AskDeny:
      ESP_LOGI(TAG, "pairing ask: PWR refuses");
      app_->answer_phone_pairing(false);
      break;
    case KeyRoute::TalkToggle:
      // The same call the home screen's TALK strip used to make: one press
      // starts a turn, the next ends it.
      app_->talk_toggle();
      ESP_LOGI(TAG, "PWR: talk");
      break;
    case KeyRoute::NoteToggle:
      if (notes_ != nullptr) notes_->on_note_button();
      break;
    case KeyRoute::MenuTapNext:
      // The menu's own grammar: CANCEL moves, TALK changes (App::settings_input).
      press(app_, hg::Button::Cancel);
      ESP_LOGI(TAG, "BOOT: menu next");
      break;
    case KeyRoute::MenuTapChange:
      press(app_, hg::Button::Talk);
      ESP_LOGI(TAG, "PWR: menu change");
      break;
    case KeyRoute::MenuOpen:
      if (app_->open_settings()) ESP_LOGI(TAG, "BOOT held: menu open");
      break;
    case KeyRoute::MenuClose:
      app_->close_settings();
      ESP_LOGI(TAG, "BOOT held: menu closed");
      break;
    case KeyRoute::CancelNote:
      if (notes_ == nullptr || !notes_->on_cancel_button()) ESP_LOGI(TAG, "BOOT held: no note to drop");
      break;
    case KeyRoute::CancelTurn:
      // A live turn, a reply or a card is up: the hold is the old CANCEL press,
      // which is what closes it on every board.
      press(app_, hg::Button::Cancel);
      ESP_LOGI(TAG, "BOOT held: cancel");
      break;
  }
}


AutoHome::Inputs ModeRouter::auto_home_inputs() const {
  AutoHome::Inputs in;
  if (app_ == nullptr) return in;  // nothing to return from
  in.at_home = auto_home_face(app_->model().screen);
  in.dozing = app_->dozing();
  bool notes_busy = false;
  if (notes_ != nullptr) notes_busy = notes_->status().phase != NoteStatus::Phase::Idle;
  // A turn, a pairing ask, a question from Hermes and an upload all suspend the
  // count: the minute is for a device somebody walked away from, not one that is
  // in the middle of something.
  const hg::Screen screen = app_->model().screen;
  const bool question = screen == hg::Screen::Prompt || screen == hg::Screen::Choice;
  in.busy = app_->in_turn() || app_->phone_ask_pending() || question || notes_busy;
  return in;
}

void ModeRouter::tick(uint32_t now_ms) {
  if (power_gpio_ >= 0) {
    const bool down = gpio_get_level(static_cast<gpio_num_t>(power_gpio_)) == 0;
    if (down == power_raw_) {
      power_stable_ = 0;
    } else if (++power_stable_ >= kDebounceTicks) {
      power_raw_ = down;
      power_stable_ = 0;
      auto_home_.interacted(now_ms);  // a press is interaction, whatever it decides
      act(down ? keys_.power_press(now_ms) : keys_.power_release(now_ms));
    }
  }
  if (record_gpio_ >= 0) {
    const bool down = gpio_get_level(static_cast<gpio_num_t>(record_gpio_)) == 0;
    if (down == record_raw_) {
      record_stable_ = 0;
    } else if (++record_stable_ >= kDebounceTicks) {
      record_raw_ = down;
      record_stable_ = 0;
      auto_home_.interacted(now_ms);
      act(down ? keys_.key_press(now_ms) : keys_.key_release(now_ms));
    }
    // A press that passes the hold threshold cancels before it is lifted.
    if (keys_.key_down()) act(keys_.tick(now_ms));
  }

  if (live_ != nullptr) live_->tick(now_ms);
  if (notes_ != nullptr) notes_->tick(now_ms);

  if (auto_home_.tick(now_ms, auto_home_inputs())) {
    ESP_LOGI(TAG, "home: %u s with no interaction", static_cast<unsigned>(kAutoHomeSeconds));
    if (app_ != nullptr) {
      app_->close_settings();  // a menu left open gives way like any other screen
      app_->go_home();
    }
  }
}

}  // namespace hgp
