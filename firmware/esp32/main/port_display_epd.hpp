// Waveshare ESP32-S3-ePaper-1.54's 200x200 e-paper: the whole panel in one
// RGB565 framebuffer (PSRAM), thresholded to 1 bit on flush and pushed with
// the panel's full or partial refresh sequence. flush() only converts the
// changed rows and marks the panel dirty; a task coalesces the bands of a
// frame and runs the update off the app task, because a refresh's BUSY wait
// can outlast the frame budget.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "board.hpp"  // EpdConfig
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hg/hal.hpp"

namespace hgp {

class EpdDisplay final : public hg::Display {
 public:
  bool begin(const EpdConfig& cfg);
  const char* controller_name() const { return "ssd1681"; }
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  bool take_redraw_request() override {
    const bool pending = repaint_.exchange(false, std::memory_order_relaxed);
    if (pending) force_full_ = true;  // the redraw lands on a fresh full refresh
    return pending;
  }
  // The 1-bit panel image, hex, one line: `screen` on the console proves what
  // the panel is showing (text and all) without a camera pointed at it. Empty
  // before begin(). Rows are MSB-first, padded to a byte.
  std::string image_hex();

 private:
  static void task(void* arg);
  void panel_init();          // power-on sequence and the full waveform
  void panel_partial_init();  // arms the partial waveform after a full refresh
  void refresh();             // one panel update from the image snapshot
  void convert(int y0, int y1); // RGB565 rows -> 1 bit per pixel
  bool wait_busy();
  void command(uint8_t cmd);
  void data(uint8_t byte);
  void write(const uint8_t* bytes, size_t len);
  void set_lut(const uint8_t* lut);

  EpdConfig cfg_{};
  spi_device_handle_t spi_ = nullptr;
  uint16_t* fb_ = nullptr;   // the UI's RGB565 framebuffer
  uint8_t* image_ = nullptr;  // next panel image, 1 bit per pixel
  uint8_t* sent_ = nullptr;   // snapshot the refresh task transmits
  SemaphoreHandle_t lock_ = nullptr;  // guards image_/dirty_
  SemaphoreHandle_t gate_ = nullptr;  // flush signals: rows went dirty
  std::atomic<bool> repaint_{false};  // a refresh failed: redraw the screen
  std::atomic<bool> force_full_{false};
  bool dirty_ = false;         // the panel does not show image_ yet
  bool partial_ = false;       // the partial waveform is armed
  int partials_ = 0;           // partial updates since the last full one
};

}  // namespace hgp
