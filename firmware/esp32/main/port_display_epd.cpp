// See port_display_epd.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_display_epd.hpp"
#if CONFIG_HG_BOARD_WS_EPAPER_154

#include <algorithm>
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
constexpr int kFullEvery = 20;
// The vendor's driver comments name 10 MHz as the panel's clock; a full
// frame is 5,000 bytes either way, about 4 ms on the wire.
constexpr int kSpiHz = 10 * 1000 * 1000;
constexpr int64_t kRailSettleMs = 50;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// Bytes staged into internal RAM per SPI transaction: small enough that the
// allocation is never the problem, large enough to keep the bus busy.
constexpr size_t kTxChunk = 1024;

}  // namespace

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
  tx_ = static_cast<uint8_t*>(heap_caps_malloc(kTxChunk, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!fb_ || !image_ || !sent_ || !tx_) {
    ESP_LOGE(TAG, "no memory for the %ux%u framebuffer", cfg.width, cfg.height);
    return false;
  }
  memset(image_, 0, image_bytes);

  // Both rails on this board conduct LOW; the audio rail is already up
  // (power_rails_on), this one gates the panel.
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
  const size_t image_bytes = static_cast<size_t>(cfg_.width) * cfg_.height / 8;
  xSemaphoreTake(lock_, portMAX_DELAY);
  memcpy(sent_, image_, image_bytes);
  dirty_ = false;
  xSemaphoreGive(lock_);

  // Full on the first frame, every kFullEvery partial ones, and whenever the
  // UI asked for a repaint after a failed transfer. The sequence mirrors the
  // vendor's boot order: full update into both RAM planes, then arm the
  // partial waveform for the updates that follow.
  const bool full = force_full_.exchange(false) || !partial_ || partials_ >= kFullEvery;
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
    panel_partial_init();
    partial_ = true;
    partials_ = 0;
    ESP_LOGI(TAG, "full refresh");
    return;
  }
  command(0x24);
  if (!write(sent_, image_bytes)) {
    repaint_ = true;  // the panel keeps the frame it has; redraw it in full
    force_full_ = true;
    return;
  }
  command(0x22);
  data(0xcf);
  command(0x20);
  wait_busy();
  ++partials_;
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
