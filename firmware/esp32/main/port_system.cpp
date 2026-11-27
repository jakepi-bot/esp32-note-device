#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <string>
#include <ctime>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_timer.h"

namespace hgp {

uint32_t EspSystem::now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint64_t EspSystem::now_us() { return static_cast<uint64_t>(esp_timer_get_time()); }

bool local_wall_time(hg::WallTime& out) {
  const time_t t = time(nullptr);
  struct tm tm{};
  // A fresh boot without RTC and network still reads 1970; a real date is
  // 2025 or later.
  if (!localtime_r(&t, &tm) || tm.tm_year + 1900 < 2025) return false;
  out.year = static_cast<uint16_t>(tm.tm_year + 1900);
  out.month = static_cast<uint8_t>(tm.tm_mon + 1);
  out.day = static_cast<uint8_t>(tm.tm_mday);
  out.weekday = static_cast<uint8_t>(tm.tm_wday);
  out.hour = static_cast<uint8_t>(tm.tm_hour);
  out.minute = static_cast<uint8_t>(tm.tm_min);
  out.second = static_cast<uint8_t>(tm.tm_sec);
  return true;
}

void EspSystem::random_bytes(uint8_t* out, size_t len) {
  // Hardware RNG; entropy is good once the radio is up, and the device key is
  // generated after Wi-Fi starts (see main.cpp).
  esp_fill_random(out, len);
}

void EspSystem::log(hg::LogLevel level, std::string_view message) {
  static const char* TAG = "hg";
  std::string msg(message);
  switch (level) {
    case hg::LogLevel::Debug: ESP_LOGD(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Info: ESP_LOGI(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Warn: ESP_LOGW(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Error: ESP_LOGE(TAG, "%s", msg.c_str()); break;
  }
}

namespace {
// Light sleep taken since the doze began, for the log line that ends it.
std::atomic<int64_t> g_slept_us{0};
std::atomic<int64_t> g_slept_total_us{0};  // since boot, for the key timing
std::atomic<uint32_t> g_sleeps{0};
std::atomic<bool> g_dozing{false};

// A resting chip is asleep, so its console cannot answer the very question worth
// asking ("how often did something wake you?"): the USB port is not a wake
// source. These two counters live in RTC memory, which survives a software
// reset, so a bench can zero them, let the device rest, reset the chip over
// USB, and still read the number it came for.
RTC_NOINIT_ATTR uint32_t g_wakes_rtc;
RTC_NOINIT_ATTR uint32_t g_wakes_magic;
// What woke it. A timer wake is a task's timeout (a poll); a GPIO wake is a pin
// asserting (a key, or something the board drives) - and the two have completely
// different fixes, so the count alone is not enough to act on.
RTC_NOINIT_ATTR uint32_t g_wake_timer_rtc;
RTC_NOINIT_ATTR uint32_t g_wake_gpio_rtc;
RTC_NOINIT_ATTR uint32_t g_wake_other_rtc;
constexpr uint32_t kWakesMagic = 0x57414B45u;  // 'WAKE'

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t count_sleep(int64_t slept_us, void*) {
  g_slept_us.fetch_add(slept_us, std::memory_order_relaxed);
  g_slept_total_us.fetch_add(slept_us, std::memory_order_relaxed);
  g_sleeps.fetch_add(1, std::memory_order_relaxed);
  ++g_wakes_rtc;
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: ++g_wake_timer_rtc; break;
    case ESP_SLEEP_WAKEUP_GPIO: ++g_wake_gpio_rtc; break;
    default: ++g_wake_other_rtc; break;
  }
  return ESP_OK;
}
#endif
}  // namespace

// Count light sleep without taking the "may not sleep" lock `enable_doze()` takes:
// an app that sleeps whenever it is idle still needs to know how much it slept,
// because the key timing subtracts exactly that (2026-11-24).
void keep_pins_through_sleep(const BoardConfig& b) {
  // A latch-powered board must also keep its battery latch high, or light sleep cuts the supply.
  const int pins[] = {b.amoled.cs, b.amoled.sclk, b.amoled.d0, b.amoled.d1, b.amoled.d2, b.amoled.d3,
                      b.amoled.rst, b.lcd.cs, b.lcd.dc, b.lcd.rst, b.lcd.backlight, b.touch.rst,
                      b.touch.bus.sda, b.touch.bus.scl, b.codec.pa, b.i2c.sda, b.i2c.scl,
                      b.latch_power.enabled ? b.latch_power.enable : -1};
  for (int pin : pins)
    if (pin >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
#if CONFIG_HG_BOARD_WS_EPAPER_154
  // The e-paper's rail and SPI lines keep their level too: the image needs no
  // power, but the next refresh finds the panel as it left it.
  for (int pin : {b.epd.sclk, b.epd.mosi, b.epd.cs, b.epd.dc, b.epd.rst, b.epd.pwr})
    if (pin >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
#endif
  for (const hgp::PowerRail& rail : b.power_rails)
    if (rail.gpio >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(rail.gpio));
}

void EspSystem::enable_sleep_accounting() {
#if CONFIG_PM_ENABLE && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  if (g_wakes_magic != kWakesMagic) {
    // A cold start, not a software reset: the counters start from zero, and the
    // magic says a reset from here on may keep them.
    g_wakes_magic = kWakesMagic;
    g_wakes_rtc = 0;
  }
  static bool registered = false;
  if (registered) return;
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = &count_sleep;
  registered = esp_pm_light_sleep_register_cbs(&cbs) == ESP_OK;
#endif
}

bool EspSystem::enable_doze() {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  if (awake_) return true;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = &count_sleep;
  esp_pm_light_sleep_register_cbs(&cbs);
#endif
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-awake", &awake_) != ESP_OK) {
    awake_ = nullptr;
    return false;
  }
  esp_pm_lock_acquire(awake_);
  return true;
#else
  return false;
#endif
}

bool EspSystem::dozing_now() { return g_dozing.load(std::memory_order_relaxed); }

// Light sleep since boot, in milliseconds: the key timing subtracts exactly this
// from a press, so time the loop merely took is not mistaken for sleep.
uint32_t EspSystem::slept_ms_total() {
  return static_cast<uint32_t>(g_slept_total_us.load(std::memory_order_relaxed) / 1000);
}

uint32_t EspSystem::light_sleep_exits() { return g_wakes_magic == kWakesMagic ? g_wakes_rtc : 0; }

EspSystem::WakeMix EspSystem::light_sleep_wakes() {
  WakeMix mix;
  mix.timer = g_wake_timer_rtc;
  mix.gpio = g_wake_gpio_rtc;
  mix.other = g_wake_other_rtc;
  return mix;
}

void EspSystem::reset_light_sleep_exits() {
  g_wakes_magic = kWakesMagic;
  g_wakes_rtc = 0;
  g_wake_timer_rtc = 0;
  g_wake_gpio_rtc = 0;
  g_wake_other_rtc = 0;
}

namespace {
std::atomic<bool> g_sleep_held{false};
}

void EspSystem::set_sleep_held(bool held) {
  if (g_sleep_held.exchange(held) == held) return;
  if (on_sleep_held_change) on_sleep_held_change();
}

bool EspSystem::sleep_held_now() { return g_sleep_held.load(std::memory_order_relaxed); }

void EspSystem::set_dozing(bool dozing) {
  if (!awake_ || dozing == dozing_) return;
  dozing_ = dozing;
  if (dozing) {
    g_slept_us = 0;
    g_sleeps = 0;
    doze_started_us_ = esp_timer_get_time();
    if (on_doze_start) on_doze_start();
    g_dozing = true;
    ESP_LOGI("hg.power", "dozing: light sleep allowed");
    esp_pm_lock_release(awake_);
    return;
  }
  esp_pm_lock_acquire(awake_);
  g_dozing = false;
  if (on_doze_end) on_doze_end();
  const int64_t dozed_ms = (esp_timer_get_time() - doze_started_us_) / 1000;
  const int64_t slept_ms = g_slept_us.load() / 1000;
  ESP_LOGI("hg.power", "awake after %lld s dozing: light sleep %lld s (%d%%), %u wake-ups", dozed_ms / 1000,
           slept_ms / 1000, dozed_ms > 0 ? static_cast<int>(slept_ms * 100 / dozed_ms) : 0,
           static_cast<unsigned>(g_sleeps.load()));
}

bool EspSystem::wall_time(hg::WallTime& out) { return local_wall_time(out); }

}  // namespace hgp
