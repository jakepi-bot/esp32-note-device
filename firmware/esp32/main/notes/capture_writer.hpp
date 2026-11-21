// The recording's audio, from the microphone to the card without the app loop.
//
// A note is recorded as ~13 ms chunks (77 a second). Writing each one straight
// to the card from the app task used to cost the loop whatever the card took:
// this card has answered a single 640-byte write in 1.5 s, which filled the
// event queue and made `notes stop` miss the console's reply deadline (the
// hardware QA's D4). So the app task only copies a chunk into a ring buffer,
// and a task of its own does the file work in 8 KB writes.
//
// One writer for the app's life: begin() once, then push() from the app task
// while a note records, drain() before the file is finished or dropped.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace hgp::notes {
class Capture;
}  // namespace hgp::notes

namespace hgp {

class CaptureWriter {
 public:
  // The ring is 128 KB of PSRAM: a 1.5 s card stall costs 72 KB of audio, and
  // nothing leaves the app loop while it happens.
  static constexpr size_t kRingBytes = 128 * 1024;

  bool begin();
  void bind(notes::Capture* capture) { capture_ = capture; }

  // App task, once per microphone chunk: a copy into the ring, no file call.
  void push(const int16_t* samples, size_t count);

  // Everything pushed is on the card (true) or the timeout ran out (false).
  // Call before Capture::finish()/abort(): the file must not be touched while
  // the writer holds it.
  bool drain(uint32_t timeout_ms);

  // Drops what is still buffered (an aborted recording); the writer may still
  // be finishing the write it had started, so drain() first to be exact.
  void discard();

  // Bytes the ring could not take (a card slower than the ring is deep).
  uint32_t dropped_bytes() const { return dropped_bytes_.load(std::memory_order_relaxed); }
  void reset_dropped() { dropped_bytes_.store(0, std::memory_order_relaxed); }

 private:
  static void task(void* arg);
  void run();

  notes::Capture* capture_ = nullptr;
  StreamBufferHandle_t ring_ = nullptr;
  void* storage_ = nullptr;  // the ring's PSRAM, kept for a clean teardown
  StaticStreamBuffer_t control_{};
  std::atomic<uint32_t> pending_{0};  // pushed bytes the writer has not written
  std::atomic<uint32_t> dropped_bytes_{0};
  std::atomic<bool> discarding_{false};
};

}  // namespace hgp
