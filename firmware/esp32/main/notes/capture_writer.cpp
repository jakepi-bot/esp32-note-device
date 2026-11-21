// See capture_writer.hpp.
#include "../port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "capture_writer.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "notes_capture.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.rec";
constexpr size_t kWriteBytes = 8 * 1024;  // two 4 KB FATFS sectors per call
constexpr UBaseType_t kTaskPriority = 1;
constexpr uint32_t kTaskStack = 4096;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

}  // namespace

bool CaptureWriter::begin() {
  if (ring_ != nullptr) return true;
  storage_ = heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (storage_ == nullptr) storage_ = heap_caps_malloc(kRingBytes, MALLOC_CAP_8BIT);
  if (storage_ == nullptr) {
    ESP_LOGE(TAG, "no memory for the recording ring");
    return false;
  }
  // Trigger level 1: the writer wakes as soon as a byte is there, and takes
  // whatever else arrived with it (up to kWriteBytes).
  ring_ = xStreamBufferCreateStatic(kRingBytes, 1, static_cast<uint8_t*>(storage_), &control_);
  if (ring_ == nullptr) {
    free(storage_);
    storage_ = nullptr;
    ESP_LOGE(TAG, "no stream buffer for the recording ring");
    return false;
  }
  if (xTaskCreate(&CaptureWriter::task, "hg-notes-wr", kTaskStack, this, kTaskPriority, nullptr) != pdPASS) {
    vStreamBufferDelete(ring_);
    ring_ = nullptr;
    free(storage_);
    storage_ = nullptr;
    ESP_LOGE(TAG, "no task for the recording writer");
    return false;
  }
  ESP_LOGI(TAG, "recording writer ready (%u KB ring)", static_cast<unsigned>(kRingBytes / 1024));
  return true;
}

void CaptureWriter::push(const int16_t* samples, size_t count) {
  if (ring_ == nullptr || samples == nullptr || count == 0) return;
  const size_t bytes = count * sizeof(int16_t);
  if (discarding_.load(std::memory_order_relaxed)) return;
  // Counted before the send: drain() must not see the ring empty while a chunk
  // is on its way in.
  pending_.fetch_add(static_cast<uint32_t>(bytes), std::memory_order_relaxed);
  if (xStreamBufferSend(ring_, samples, bytes, 0) != bytes) {
    // The card is behind by more than the ring holds. Keep what is buffered
    // (the note stays continuous up to the hole) and count the loss.
    pending_.fetch_sub(static_cast<uint32_t>(bytes), std::memory_order_relaxed);
    dropped_bytes_.fetch_add(static_cast<uint32_t>(bytes), std::memory_order_relaxed);
  }
}

bool CaptureWriter::drain(uint32_t timeout_ms) {
  if (ring_ == nullptr) return true;
  const int64_t deadline = now_ms() + timeout_ms;
  while (pending_.load(std::memory_order_relaxed) != 0) {
    if (now_ms() >= deadline) {
      ESP_LOGW(TAG, "%u bytes still unwritten after %u ms", static_cast<unsigned>(pending_.load()),
               static_cast<unsigned>(timeout_ms));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

void CaptureWriter::discard() {
  if (ring_ == nullptr) return;
  discarding_.store(true, std::memory_order_relaxed);
  drain(2000);  // the chunk being written still has to come back
  xStreamBufferReset(ring_);
  pending_.store(0, std::memory_order_relaxed);
  discarding_.store(false, std::memory_order_relaxed);
}

void CaptureWriter::task(void* arg) {
  static_cast<CaptureWriter*>(arg)->run();
  vTaskDelete(nullptr);
}

void CaptureWriter::run() {
  int16_t* chunk = static_cast<int16_t*>(heap_caps_malloc(kWriteBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (chunk == nullptr) chunk = static_cast<int16_t*>(heap_caps_malloc(kWriteBytes, MALLOC_CAP_8BIT));
  if (chunk == nullptr) {
    ESP_LOGE(TAG, "no memory for the writer's chunk; recordings will not be saved");
    return;
  }
  for (;;) {
    const size_t n = xStreamBufferReceive(ring_, chunk, kWriteBytes, portMAX_DELAY);
    if (n == 0) continue;
    if (!discarding_.load(std::memory_order_relaxed) && capture_ != nullptr && capture_->active()) {
      const int64_t t0 = esp_timer_get_time();
      capture_->feed(chunk, n / sizeof(int16_t));
      const int64_t took = (esp_timer_get_time() - t0) / 1000;
      if (took > 500) ESP_LOGW(TAG, "%u bytes to the card took %lld ms", static_cast<unsigned>(n), static_cast<long long>(took));
    }
    pending_.fetch_sub(static_cast<uint32_t>(n), std::memory_order_relaxed);
  }
}

}  // namespace hgp
