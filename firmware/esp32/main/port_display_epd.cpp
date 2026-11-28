// See port_display_epd.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_display_epd.hpp"
#if CONFIG_HG_BOARD_WS_EPAPER_154

#include <algorithm>
#include <atomic>
#include <cstring>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.epd";

// Full and partial waveform LUTs for the 1.54" V2 panel: the 153-byte table
// for command 0x32 plus the five bytes that follow it in Waveshare's driver.
// Register data only, no vendor code copied: Waveshare's demo for this board,
// https://github.com/waveshareteam/ESP32-S3-ePaper-1.54, commit
// 48e978d328cfd2e48193b08c10725d50f71d90d9 (components/port_bsp/port_display.cpp,
// the tables the factory firmware writes).
const uint8_t kLutFull[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x08, 0x01,
    0x00, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x22, 0x17, 0x41,
    0x00, 0x32, 0x20,
};

const uint8_t kLutPartial[159] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x02, 0x17, 0x41,
    0xB0, 0x32, 0x28,
};

// The bands of one frame arrive as separate flushes this far apart; collect
// them before touching the panel. A screen that keeps changing still waits
// kMinGapMs between updates, and a screen that changes forever refreshes at
// least every kMaxCoalesceMs once its first band is in.
constexpr int64_t kQuietMs = 80;
constexpr int64_t kMaxCoalesceMs = 250;
constexpr int64_t kMinGapMs = 300;
// The panel's BUSY line (HIGH = busy, the vendor driver's polarity). A
// refresh that never finishes is reported, not waited out forever: the next
// update then refreshes the whole panel from a known state.
constexpr int64_t kBusyTimeoutMs = 3000;
// A full refresh clears the ghosting a run of partial ones leaves behind.
// A partial waveform leaves residue; twenty of them is a visible ghost on a
// band that redraws a minute apart. Ten keeps the glass clean without turning
// every minute into a flash (2026-11-23).
// The panel on this board does not ghost: Jake ran the notes UI for an afternoon
// of partials and saw no residue (2026-11-24). Ten was the conservative default;
// sixty-four keeps the periodic clean-up for a panel that needs it without the
// full-flash every few seconds, and `epd ghost <n>` still moves it from the console.
constexpr int kFullEvery = 64;  // the default; see set_full_every()
// The vendor's driver comments name 10 MHz as the panel's clock; a full
// frame is 5,000 bytes either way, about 4 ms on the wire.
constexpr int kSpiHz = 10 * 1000 * 1000;
constexpr int64_t kRailSettleMs = 50;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// Bytes staged into internal RAM per SPI transaction: small enough that the
// allocation is never the problem, large enough to keep the bus busy.
constexpr size_t kTxChunk = 1024;

}  // namespace

void EpdDisplay::panel_power(bool on) {
  if (pwr_pin_ < 0 || on == powered_) return;
  // Both rails on this board conduct LOW, so "on" is the configured level and
  // "off" is its opposite.
  const int level = on ? (pwr_on_level_ ? 1 : 0) : (pwr_on_level_ ? 0 : 1);
  gpio_set_level(static_cast<gpio_num_t>(pwr_pin_), level);
  powered_ = on;
  // The rail is a boost converter: every edge is a click a person can hear, so
  // transitions are logged rather than only implied by a frame.
  ESP_LOGI(TAG, "panel rail %s", on ? "up" : "down");
  if (on) vTaskDelay(pdMS_TO_TICKS(kRailSettleMs));
}

bool EpdDisplay::begin(const EpdConfig& cfg) {
  cfg_ = cfg;
  const size_t pixels = static_cast<size_t>(cfg.width) * cfg.height;
  const size_t image_bytes = pixels / 8;  // 200x200: 5,000 bytes, 25 per row
  fb_ = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fb_) fb_ = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_8BIT));
  image_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!image_) image_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_8BIT));
  sent_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!sent_) sent_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_8BIT));
  shown_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!shown_) shown_ = static_cast<uint8_t*>(heap_caps_malloc(image_bytes, MALLOC_CAP_8BIT));
  tx_ = static_cast<uint8_t*>(heap_caps_malloc(kTxChunk, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!fb_ || !image_ || !sent_ || !shown_ || !tx_) {
    ESP_LOGE(TAG, "no memory for the %ux%u framebuffer", cfg.width, cfg.height);
    return false;
  }
  memset(image_, 0, image_bytes);
  memset(shown_, 0, image_bytes);

  // Both rails on this board conduct LOW; the audio rail is already up
  // (power_rails_on), this one gates the panel.
  pwr_pin_ = cfg.pwr;
  pwr_on_level_ = cfg.pwr_on_level;
  powered_ = true;
  if (cfg.pwr >= 0) {
    gpio_config_t rail = {};
    rail.pin_bit_mask = 1ULL << cfg.pwr;
    rail.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&rail) != ESP_OK) return false;
    gpio_set_level(static_cast<gpio_num_t>(cfg.pwr), cfg.pwr_on_level ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(kRailSettleMs));
  }
  const int pins[] = {cfg.cs, cfg.dc, cfg.rst};
  gpio_config_t out = {};
  out.mode = GPIO_MODE_OUTPUT;
  for (int pin : pins)
    if (pin >= 0) out.pin_bit_mask |= 1ULL << pin;
  if (gpio_config(&out) != ESP_OK) return false;
  gpio_set_level(static_cast<gpio_num_t>(cfg.cs), 1);
  gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 1);
  gpio_config_t busy = {};
  busy.pin_bit_mask = 1ULL << cfg.busy;
  busy.mode = GPIO_MODE_INPUT;
  if (gpio_config(&busy) != ESP_OK) return false;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = cfg.mosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = cfg.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(image_bytes);
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
    return false;
  }
  spi_device_interface_config_t dev = {};
  dev.spics_io_num = -1;  // CS is this driver's GPIO, asserted per byte burst
  dev.clock_speed_hz = kSpiHz;
  dev.mode = 0;
  dev.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &dev, &spi_) != ESP_OK) {
    ESP_LOGE(TAG, "spi device init failed");
    return false;
  }

  lock_ = xSemaphoreCreateMutex();
  gate_ = xSemaphoreCreateBinary();
  if (!lock_ || !gate_) return false;

  panel_init();
  if (xTaskCreate(&EpdDisplay::task, "epd", 4096, this, 2, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "refresh task did not start");
    return false;
  }
  ESP_LOGI(TAG, "%ux%u e-paper ready: sclk=%d mosi=%d cs=%d dc=%d rst=%d busy=%d pwr=%d",
           cfg.width, cfg.height, cfg.sclk, cfg.mosi, cfg.cs, cfg.dc, cfg.rst, cfg.busy, cfg.pwr);
  return true;
}

hg::DisplayInfo EpdDisplay::info() const {
  hg::DisplayInfo i;
  i.width = cfg_.width;
  i.height = cfg_.height;
  // The conversion reads logical RGB565 values, so the canvas writes them
  // plain; nothing is sent to the panel in RGB565.
  i.swap_bytes = false;
  i.has_backlight = false;
  // The image stays with the panel unpowered and there is no light to turn off:
  // the core may rest on this frame and let the chip doze (App::power_tick).
  i.bistable = true;
  i.round = false;
  // Paper, not a lit panel: the UI keeps its dark background (emissive stays
  // off), and nothing dims or blanks it (set_backlight is the no-op default).
  i.emissive = false;
  return i;
}

std::string EpdDisplay::image_slice(size_t first, size_t count) {
  if (!image_ || !lock_) return {};
  const size_t bytes = static_cast<size_t>(cfg_.width) * cfg_.height / 8;
  if (first >= bytes) return {};
  if (count == 0 || first + count > bytes) count = bytes - first;
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  xSemaphoreTake(lock_, portMAX_DELAY);
  for (size_t i = first; i < first + count; ++i) {
    out.push_back(kHex[image_[i] >> 4]);
    out.push_back(kHex[image_[i] & 0x0f]);
  }
  xSemaphoreGive(lock_);
  return out;
}

void EpdDisplay::flush(uint16_t y0, uint16_t y1) {
  // A frame that arrives while the rail is down (idle ended) wakes it: the panel
  // lost its previous-frame RAM with the power, so ask for a full waveform
  // whatever the caller wanted - a partial over a cold panel draws nothing.
  if (!powered_) {
    panel_power(true);
    request_full_refresh();
  }
  if (!fb_ || !image_) return;
  if (y1 > cfg_.height) y1 = cfg_.height;
  if (y0 >= y1) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  convert(y0, y1);
  dirty_ = true;
  xSemaphoreGive(lock_);
  xSemaphoreGive(gate_);
}

void EpdDisplay::task(void* arg) {
  EpdDisplay* self = static_cast<EpdDisplay*>(arg);
  int64_t last_refresh = 0;
  for (;;) {
    xSemaphoreTake(self->gate_, portMAX_DELAY);
    // The frame's remaining bands land within kQuietMs of the first one;
    // a screen that redraws continuously stops waiting at kMaxCoalesceMs.
    const int64_t first = now_ms();
    while (self->dirty_ && now_ms() - first < kMaxCoalesceMs) {
      if (xSemaphoreTake(self->gate_, pdMS_TO_TICKS(kQuietMs)) != pdTRUE) break;
    }
    while (self->dirty_) {
      const int64_t wait = kMinGapMs - (now_ms() - last_refresh);
      if (wait <= 0) break;
      xSemaphoreTake(self->gate_, pdMS_TO_TICKS(wait));
    }
    if (!self->dirty_) continue;
    self->refresh();
    last_refresh = now_ms();
    while (xSemaphoreTake(self->gate_, 0) == pdTRUE) {
    }  // signals from bands this refresh already sent
  }
}

void EpdDisplay::refresh() {
  update_once();
  // Counted after the panel's BUSY line has cleared (or the attempt was given
  // up on): wait_idle() reads this to tell a finished update from a queued one.
  updates_.fetch_add(1, std::memory_order_relaxed);
}

bool EpdDisplay::wait_idle(uint32_t after, uint32_t timeout_ms) {
  const int64_t start = esp_timer_get_time();
  for (;;) {
    bool queued;
    xSemaphoreTake(lock_, portMAX_DELAY);
    queued = dirty_;
    xSemaphoreGive(lock_);
    // Finished means: the panel is not driving BUSY, nothing is queued behind
    // it, and an update ran since the caller flushed. Without the count, a
    // flush the task had not picked up yet would read as an idle panel.
    if (!queued && updates_.load(std::memory_order_relaxed) != after &&
        gpio_get_level(static_cast<gpio_num_t>(cfg_.busy)) == 0) {
      return true;
    }
    if ((esp_timer_get_time() - start) / 1000 > static_cast<int64_t>(timeout_ms)) {
      ESP_LOGW(TAG, "the panel is still busy after %u ms", static_cast<unsigned>(timeout_ms));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void EpdDisplay::update_once() {
  const size_t image_bytes = static_cast<size_t>(cfg_.width) * cfg_.height / 8;
  const size_t row_bytes = static_cast<size_t>(cfg_.width) / 8;
  // What actually moved, as a row band. A partial waveform over the whole panel
  // costs the same as a full one minus the flash, so the band is where the
  // saving is: a 20-row footer on a 200-row panel is a tenth of the drive time
  // and the same tenth of the charge (2026-11-23).
  int band0 = -1, band1 = -1;
  xSemaphoreTake(lock_, portMAX_DELAY);
  for (int y = 0; y < cfg_.height; ++y) {
    if (std::memcmp(image_ + static_cast<size_t>(y) * row_bytes,
                    sent_ + static_cast<size_t>(y) * row_bytes, row_bytes) != 0) {
      if (band0 < 0) band0 = y;
      band1 = y;
    }
  }
  memcpy(sent_, image_, image_bytes);
  dirty_ = false;
  xSemaphoreGive(lock_);
  // Nothing moved: the panel is bistable and an update that changes no pixel is
  // charge spent on holding the same picture.
  if (band0 < 0) return;

  // Temporary energy instrumentation (build flag CONFIG_HG_EPD_ENERGY_LOG):
  // stamp this refresh so the partial and full paths can be told apart and
  // counted for the mAh measurement. Strictly additive; no refresh behaviour
  // changes. The marker carries the driving time and the rail state the bench
  // meter will be put on, and the running totals the report consumes.
  const bool is_full = force_full_.load(std::memory_order_relaxed) || !partial_ || partials_ >= full_every_;
  #if CONFIG_HG_EPD_ENERGY_LOG
  {
    const int rows = band1 - band0 + 1;
    epd_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    epd_rows_driven_total_.fetch_add(static_cast<uint32_t>(rows), std::memory_order_relaxed);
    epd_drive_ms_total_.fetch_add(static_cast<uint32_t>(now_ms()), std::memory_order_relaxed);
    last_refresh_rows_ = rows;
    last_refresh_ms_ = now_ms();
    last_refresh_full_ = is_full;
    ESP_LOGI(TAG, "[EPD] refresh seq=%lu full=%d rows=%d ts=%ld ms rail_up=%d",
             static_cast<unsigned>(epd_refresh_count_.load(std::memory_order_relaxed)), is_full ? 1 : 0,
             rows, static_cast<long>(now_ms()), powered_ ? 1 : 0);
  }
  #endif

  // Full on the first frame, every kFullEvery partial ones, and whenever the
  // UI asked for a repaint after a failed transfer. The sequence mirrors the
  // vendor's boot order: full update into both RAM planes, then arm the
  // partial waveform for the updates that follow.
  const bool full = force_full_.exchange(false) || !partial_ || partials_ >= full_every_;
  if (full) {
    panel_init();
    command(0x24);
    bool taken = write(sent_, image_bytes);
    command(0x26);
    taken = write(sent_, image_bytes) && taken;
    if (!taken) {
      // Half a frame in the panel's RAM would print as garbage: do not trigger
      // the update, and let the UI redraw so the next pass is a clean full one.
      partial_ = false;
      repaint_ = true;
      force_full_ = true;
      return;
    }
    command(0x22);
    data(0xc7);
    command(0x20);
    if (!wait_busy()) {
      partial_ = false;  // the next update starts from the full sequence
      return;
    }
    memcpy(shown_, sent_, image_bytes);
    panel_partial_init();
    partial_ = true;
    partials_ = 0;
    ESP_LOGI(TAG, "full refresh");
    return;
  }
  // A partial addresses the frame exactly as panel_init does: the window is
  // written start-row 199 -> end-row 0, the cursor goes on row 199, and the
  // image is written forward. That is the panel's own order. Measured on the
  // unit (2026-11-23): the same trigger with the window expressed 0 -> 199
  // paints nothing at all, and so does any band taller than half the page in
  // that form - which is why a page-size hand-over used to reach the glass only
  // on the next full refresh. In this form a whole page paints in 589 ms, and a
  // one-row change costs the same 589 ms: the partial waveform's duration on
  // this panel does not depend on how many rows it drives, so there is nothing
  // to win by narrowing the window and everything to lose by getting the
  // addressing wrong.
  const int64_t t0 = now_ms();
  if (!partial_frame()) {
    repaint_ = true;  // the panel keeps the frame it has; redraw it in full
    force_full_ = true;
    return;
  }
  memcpy(shown_, sent_, image_bytes);
  ++partials_;
  ESP_LOGI(TAG, "partial %d rows (%d..%d) of %d, %lld ms of driving", band1 - band0 + 1, band0, band1,
           cfg_.height, static_cast<long long>(now_ms() - t0));
}

bool EpdDisplay::partial_frame() {
  const size_t image_bytes = static_cast<size_t>(cfg_.width) * cfg_.height / 8;
  // 0x26 carries the frame the panel is showing; the partial waveform is a
  // transition between the two planes, so both have to be in the panel's RAM.
  auto window = [&]() { set_window(cfg_.height - 1, 0); };
  window();
  command(0x24);
  if (!write(sent_, image_bytes)) return false;
  window();
  command(0x26);
  if (!write(shown_, image_bytes)) return false;
  command(0x22);
  data(0xcf);
  command(0x20);
  return wait_busy();
}

void EpdDisplay::set_window(int y0, int y1) {
  command(0x44);
  data(0x00);
  data(static_cast<uint8_t>(cfg_.width / 8 - 1));
  command(0x45);
  data(static_cast<uint8_t>(y0 & 0xff));
  data(static_cast<uint8_t>((y0 >> 8) & 0xff));
  data(static_cast<uint8_t>(y1 & 0xff));
  data(static_cast<uint8_t>((y1 >> 8) & 0xff));
  command(0x4e);
  data(0x00);
  command(0x4f);
  data(static_cast<uint8_t>(y0 & 0xff));
  data(static_cast<uint8_t>((y0 >> 8) & 0xff));
}

void EpdDisplay::convert(int y0, int y1) {
  const int width = cfg_.width;
  for (int y = y0; y < y1; ++y) {
    const uint16_t* row = fb_ + static_cast<size_t>(y) * width;
    uint8_t* out = image_ + static_cast<size_t>(y) * (width / 8);
    for (int x = 0; x < width; ++x) {
      const uint16_t px = row[x];
      const int r = ((px >> 11) & 0x1f) * 255 / 31;
      const int g = ((px >> 5) & 0x3f) * 255 / 63;
      const int b = (px & 0x1f) * 255 / 31;
      // Luma on the expanded channels; a set bit prints white on this panel,
      // so the UI's dark background becomes black ink and its light text white.
      const bool white = r * 299 + g * 587 + b * 114 >= 128 * 1000;
      const uint8_t mask = static_cast<uint8_t>(1u << (7 - (x & 7)));
      if (white) out[x >> 3] |= mask;
      else out[x >> 3] &= static_cast<uint8_t>(~mask);
    }
  }
}

// ------------------------------------------------------------------ panel

void EpdDisplay::command(uint8_t cmd) {
  gpio_set_level(static_cast<gpio_num_t>(cfg_.dc), 0);
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 0);
  spi_transaction_t t = {};
  t.length = 8;
  t.tx_buffer = &cmd;
  spi_device_polling_transmit(spi_, &t);
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 1);
}

void EpdDisplay::data(uint8_t byte) {
  gpio_set_level(static_cast<gpio_num_t>(cfg_.dc), 1);
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 0);
  spi_transaction_t t = {};
  t.length = 8;
  t.tx_buffer = &byte;
  spi_device_polling_transmit(spi_, &t);
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 1);
}

bool EpdDisplay::write(const uint8_t* bytes, size_t len) {
  // Slice through the internal staging buffer: `sent_` and every other image
  // buffer are in PSRAM, and the SPI driver cannot DMA from it, so a write of
  // the whole 5,000-byte frame asked internal RAM for a temporary buffer of the
  // same size on every update. Under a Wi-Fi transfer the largest free internal
  // block falls to a few KiB, that request failed with ESP_ERR_NO_MEM, and the
  // panel quietly kept the previous frame. A fixed 1 KiB buffer never asks.
  gpio_set_level(static_cast<gpio_num_t>(cfg_.dc), 1);
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 0);
  for (size_t off = 0; off < len; off += kTxChunk) {
    const size_t n = std::min(kTxChunk, len - off);
    memcpy(tx_, bytes + off, n);
    spi_transaction_t t = {};
    t.length = n * 8;
    t.tx_buffer = tx_;
    if (spi_device_polling_transmit(spi_, &t) != ESP_OK) {
      gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 1);
      ESP_LOGE(TAG, "the panel took %u of %u bytes; the frame is redrawn", static_cast<unsigned>(off),
               static_cast<unsigned>(len));
      return false;
    }
  }
  gpio_set_level(static_cast<gpio_num_t>(cfg_.cs), 1);
  return true;
}

bool EpdDisplay::wait_busy() {
  // The panel raises BUSY as it starts an update; give it a moment to do so
  // before polling, then never wait forever on a panel that does not answer.
  vTaskDelay(pdMS_TO_TICKS(2));
  const int64_t start = esp_timer_get_time();
  while (gpio_get_level(static_cast<gpio_num_t>(cfg_.busy)) == 1) {
    if ((esp_timer_get_time() - start) / 1000 > kBusyTimeoutMs) {
      ESP_LOGE(TAG, "BUSY stuck high for %lld ms; the screen is redrawn from scratch",
               static_cast<long long>(kBusyTimeoutMs));
      repaint_ = true;
      force_full_ = true;
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

void EpdDisplay::set_lut(const uint8_t* lut) {
  command(0x32);
  write(lut, 153);
  wait_busy();
  command(0x3f);
  data(lut[153]);
  command(0x03);
  data(lut[154]);
  command(0x04);
  data(lut[155]);
  data(lut[156]);
  data(lut[157]);
  command(0x2c);
  data(lut[158]);
}

void EpdDisplay::panel_init() {
  // Hardware reset, then the vendor's register sequence: driver output
  // covering 200 rows, data entry down the columns, the full window, border
  // and temperature, then the full waveform.
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  wait_busy();
  command(0x12);  // SWRESET
  wait_busy();
  command(0x01);
  data(0xc7);
  data(0x00);
  data(0x01);
  command(0x11);
  data(0x01);
  command(0x44);  // window: all 200 columns (0..199 >> 3)
  data(0x00);
  data(24);
  command(0x45);
  data(199);
  data(0x00);
  data(0x00);
  data(0x00);
  command(0x3c);
  data(0x01);
  command(0x18);
  data(0x80);
  command(0x22);  // load the temperature and waveform setting
  data(0xb1);
  command(0x20);
  wait_busy();
  command(0x4e);
  data(0x00);
  command(0x4f);
  data(199);
  data(0x00);
  wait_busy();
  set_lut(kLutFull);
}

void EpdDisplay::panel_partial_init() {
  // Same reset, then the partial waveform and its border; run it once after
  // each full refresh, because that sequence reloads the full one.
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  wait_busy();
  set_lut(kLutPartial);
  command(0x37);
  data(0x00);
  data(0x00);
  data(0x00);
  data(0x00);
  data(0x00);
  data(0x40);
  data(0x00);
  data(0x00);
  data(0x00);
  data(0x00);
  command(0x3c);
  data(0x80);
  command(0x22);
  data(0xc0);
  command(0x20);
  wait_busy();
}

}  // namespace hgp

#endif  // CONFIG_HG_BOARD_WS_EPAPER_154
