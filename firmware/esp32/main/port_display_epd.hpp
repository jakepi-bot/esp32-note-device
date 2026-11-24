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
  // The next update uses the full waveform however few partials have gone by:
  // for a screen that takes the whole panel over from another one, where a
  // partial leaves the previous image ghosted through the new one.
  void request_full_refresh() { force_full_ = true; }
  // The panel's rail: both rails on this board conduct LOW. It is raised only
  // around an update - an e-paper panel holds its picture with the rail down,
  // and a powered panel plus its booster draws for nothing.
  void panel_power(bool on);
  // How many partial updates may run before a full one clears the residue. 1
  // means every frame is a full refresh (no ghosting, most charge); the default
  // is 10. The bench and a demo can set it from the console.
  void set_full_every(int n) { full_every_ = n < 1 ? 1 : (n > 100 ? 100 : n); }
  int full_every() const { return full_every_; }

  // Panel updates finished since boot. Sample it, flush, then wait_idle(): the
  // number is how a caller tells "the update my frame asked for has run" from
  // "the panel is quiet because nothing was queued yet".
  uint32_t update_count() const { return updates_.load(std::memory_order_relaxed); }
  // Blocks until an update that started after `after` has finished and the
  // panel has no queued frame. Returns false if it did not within `timeout_ms`.
  // The one caller is the sleeping face: the PWR hold's frame is only worth
  // drawing if it reaches the panel before the latch cuts the rail, so the wait
  // is bounded by what the hardware leaves, not by what the panel would like.
  bool wait_idle(uint32_t after, uint32_t timeout_ms);

  // The 1-bit panel image, hex, one line: `screen` on the console proves what
  // the panel is showing (text and all) without a camera pointed at it. Empty
  // before begin(). Rows are MSB-first, padded to a byte.
  std::string image_hex() { return image_slice(0, 0); }
  // A slice of that image, for a console that cannot print 10,000 characters in
  // one write (the USB-serial send buffer takes about 4,000 a call). count 0
  // means "to the end"; the range is clamped, so a caller can walk the image.
  std::string image_slice(size_t first, size_t count);

 private:
  static void task(void* arg);
  void panel_init();          // power-on sequence and the full waveform
  void panel_partial_init();  // arms the partial waveform after a full refresh
  void refresh();             // one panel update from the image snapshot
  void update_once();         // refresh's body; refresh counts the update
  void convert(int y0, int y1); // RGB565 rows -> 1 bit per pixel
  bool wait_busy();
  void command(uint8_t cmd);
  void data(uint8_t byte);
  bool write(const uint8_t* bytes, size_t len);  // false: the panel did not take it
  void set_lut(const uint8_t* lut);

  EpdConfig cfg_{};
  spi_device_handle_t spi_ = nullptr;
  uint16_t* fb_ = nullptr;   // the UI's RGB565 framebuffer
  uint8_t* image_ = nullptr;  // next panel image, 1 bit per pixel
  uint8_t* sent_ = nullptr;   // snapshot the refresh task transmits
  // The panel image lives in PSRAM, which this chip's SPI cannot DMA from: the
  // driver would ask internal RAM for a private buffer the size of each write
  // (5,000 bytes) and gives up when the largest free block is smaller. One
  // fixed internal buffer, written through in kTxChunk slices, asks for none.
  uint8_t* tx_ = nullptr;     // internal DMA staging for a panel write
  SemaphoreHandle_t lock_ = nullptr;  // guards image_/dirty_
  SemaphoreHandle_t gate_ = nullptr;  // flush signals: rows went dirty
  std::atomic<bool> repaint_{false};  // a refresh failed: redraw the screen
  std::atomic<bool> force_full_{false};
  std::atomic<uint32_t> updates_{0};  // updates finished (wait_idle's clock)
  bool dirty_ = false;         // the panel does not show image_ yet
  bool partial_ = false;       // the partial waveform is armed
  int partials_ = 0;           // partial updates since the last full one
  int full_every_ = 10;  // the .cpp's kFullEvery; the console can move it
};

}  // namespace hgp
