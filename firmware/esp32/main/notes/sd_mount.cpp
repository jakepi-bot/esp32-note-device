// See sd_mount.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "notes/sd_mount.hpp"

#include "board.hpp"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"

namespace hgp::sd_mount {
namespace {

constexpr const char* TAG = "hg.sd";
constexpr const char* kMountPoint = "/sdcard";
// open files the VFS allows across both users: hermes.log, a recording.wav,
// its note.json, and a little headroom for a scan.
constexpr int kMaxFiles = 8;
// The card's own DMA staging buffer: 4 sectors, 2 KiB.
//
// Every file this board reads or writes lands in a PSRAM buffer (the notes'
// 128 KB capture ring, the 64 KB upload buffer, the e-paper image), and this
// chip cannot DMA out of PSRAM, so without a buffer here the sdmmc host
// allocates a temporary internal DMA buffer for every read and write and frees
// it again. During a Wi-Fi upload that allocation is what runs the internal
// heap out: measured on this board with a 60 s / 2.8 MB note, `dma_free` 4,119 B
// but `dma_largest` 368 B when the body started streaming, and
//
//   E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
//   E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
//
// which the note read as a short file and retried 10 s later. One buffer,
// allocated at mount and kept, makes the card path independent of the heap's
// fragmentation: the 2 KiB is the largest chunk the allocator was actually
// getting during a transfer anyway (it halves from 8 KiB down to one sector),
// so the reads stay 4 blocks per transaction, and the card can no longer lose
// an upload to a heap it does not own.
constexpr size_t kSectorBytes = 512;
constexpr size_t kDmaBlocks = 4;
constexpr size_t kDmaBytes = kSectorBytes * kDmaBlocks;

SemaphoreHandle_t g_lock = nullptr;
SdConfig g_cfg{};
bool g_configured = false;
sdmmc_card_t* g_card = nullptr;
bool g_mounted = false;
std::string g_last_error;
uint32_t g_last_attempt_ms = 0;
uint8_t* g_dma_buf = nullptr;

void lock_init() {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
}

}  // namespace

void configure(const SdConfig& sd) {
  lock_init();
  g_cfg = sd;
  g_configured = true;
}

bool mounted() { return g_mounted; }

sdmmc_card_t* card() { return g_card; }

bool ensure(bool format_if_needed, std::string* error) {
  lock_init();
  if (!g_configured || !g_cfg.enabled) {
    if (error) *error = "this board has no card slot";
    return false;
  }
  if (g_mounted) return true;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
    if (error) *error = "the card is busy";
    return false;
  }
  // Two callers (the log mirror's task and the app loop) probe on their own
  // clocks; one attempt per second is enough for both, and a format always
  // runs. A second, not ten: a card that is slow to answer after power-on is
  // then found in the first second of a boot and not the thirty-first.
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  if (!format_if_needed && g_last_attempt_ms != 0 && now - g_last_attempt_ms < 1000) {
    if (error) *error = g_last_error.empty() ? std::string("no card") : g_last_error;
    xSemaphoreGive(g_lock);
    return false;
  }
  g_last_attempt_ms = now;
  if (g_mounted) {
    xSemaphoreGive(g_lock);
    return true;
  }

  // Slot 0 for the card: slot 1 carries the C6 companion's SDIO link
  // (CONFIG_ESP_HOSTED_SDIO_SLOT_1), and SDMMC_HOST_DEFAULT() also picks 1.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  // The one internal DMA buffer both directions stage through (kDmaBlocks).
  // Allocated once, before the mount: a failed allocation leaves the driver's
  // own per-call buffer in place and says so, rather than stopping the mount.
  if (!g_dma_buf) {
    g_dma_buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, kDmaBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!g_dma_buf) ESP_LOGW(TAG, "no internal DMA buffer for the card: a busy transfer may read short");
  }
  host.dma_aligned_buffer = g_dma_buf;
  host.unaligned_multi_block_rw_max_chunk_size = g_dma_buf ? kDmaBlocks : 0;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  // 4-bit where the board routes all four data lines (this board's slot has
  // only CLK, CMD and D0 wired: 1-bit).
  slot.width = g_cfg.d1 >= 0 ? 4 : 1;
  slot.clk = static_cast<gpio_num_t>(g_cfg.clk);
  slot.cmd = static_cast<gpio_num_t>(g_cfg.cmd);
  slot.d0 = static_cast<gpio_num_t>(g_cfg.d0);
  slot.d1 = static_cast<gpio_num_t>(g_cfg.d1);
  slot.d2 = static_cast<gpio_num_t>(g_cfg.d2);
  slot.d3 = static_cast<gpio_num_t>(g_cfg.d3);
  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = format_if_needed;  // only on `notes format`
  mount_cfg.max_files = kMaxFiles;
  // 16 KiB clusters: the default 512 B volume would take minutes to format
  // over the card's 1-bit SDIO link, and FAT32 needs the larger unit anyway.
  mount_cfg.allocation_unit_size = 16 * 1024;
  // No card is the common case, and the caller retries on its own clock: keep
  // IDF's sdmmc/fatfs tags quiet around the attempt and say it once ourselves.
  esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
  esp_log_level_set("SD_HOST", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
  sdmmc_card_t* card = nullptr;
  const int64_t began = esp_timer_get_time();
  esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card);
  if (err != ESP_OK) {
    // A card that did not answer at 20 MHz often answers at the probing clock:
    // 1-bit wiring, a long ribbon and a slow cell all show up here, and one
    // second attempt at boot is cheaper than a card that is not found for 30 s.
    card = nullptr;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card);
  }
  const int64_t took_ms = (esp_timer_get_time() - began) / 1000;
  esp_log_level_set("sdmmc_cmd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_periph", ESP_LOG_WARN);
  esp_log_level_set("SD_HOST", ESP_LOG_WARN);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
  xSemaphoreGive(g_lock);
  if (err != ESP_OK) {
    // "no FAT32 volume" is the card answering with nothing to mount; anything
    // else is a card that did not answer at all.
    const std::string why =
        err == ESP_FAIL ? std::string("the card has no FAT32 volume") : std::string("card error: ") + esp_err_to_name(err);
    if (error) *error = why;
    // Once per reason, not once per attempt: the caller retries on its clock.
    if (why != g_last_error) {
      g_last_error = why;
      ESP_LOGW(TAG, "no card mounted: %s (after %d ms)", why.c_str(), static_cast<int>(took_ms));
    }
    return false;
  }
  g_last_error.clear();
  g_card = card;
  g_mounted = true;
  ESP_LOGI(TAG, "mounted %s (%s) in %d ms at %d kHz%s", kMountPoint, card->cid.name, static_cast<int>(took_ms),
           host.max_freq_khz, format_if_needed ? ", formatted" : "");
  return true;
}

bool space(uint64_t* total_bytes, uint64_t* free_bytes) {
  lock_init();
  if (!g_mounted) return false;
  uint64_t total = 0, free_now = 0;
  // A full FAT scan on the card's 1-bit link: not something to do per loop pass.
  if (esp_vfs_fat_info(kMountPoint, &total, &free_now) != ESP_OK) return false;
  if (total_bytes) *total_bytes = total;
  if (free_bytes) *free_bytes = free_now;
  return true;
}

bool format(std::string* error) {
  lock_init();
  if (!g_configured || !g_cfg.enabled) {
    if (error) *error = "this board has no card slot";
    return false;
  }
  // A format is asked for by a person, so the card has to be up first: an
  // unmounted volume is mounted here (not formatted - the point is the format
  // below, not a recovery) and then rewritten like any other.
  if (!g_mounted && !ensure(true, error)) return false;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
    if (error) *error = "the card is busy";
    return false;
  }
  // In place: the VFS unmounts the FAT volume, writes a new one at the mount's
  // own cluster size, and mounts it back. It must not run with a file open, which
  // is why the caller only offers it from an idle device.
  ESP_LOGW(TAG, "formatting %s: every note on it goes", kMountPoint);
  const int64_t began = esp_timer_get_time();
  const esp_err_t err = esp_vfs_fat_sdcard_format(kMountPoint, g_card);
  const int64_t took_ms = (esp_timer_get_time() - began) / 1000;
  if (err != ESP_OK) {
    // The call recycles its own resources when the format fails, so the mount is
    // gone: report it and let the next ensure() rebuild it.
    g_card = nullptr;
    g_mounted = false;
    g_last_attempt_ms = 0;
    xSemaphoreGive(g_lock);
    const std::string why = std::string("format failed: ") + esp_err_to_name(err);
    if (error) *error = why;
    ESP_LOGE(TAG, "%s (after %d ms)", why.c_str(), static_cast<int>(took_ms));
    return false;
  }
  xSemaphoreGive(g_lock);
  g_last_error.clear();
  ESP_LOGW(TAG, "formatted %s in %d ms", kMountPoint, static_cast<int>(took_ms));
  return true;
}

void release() {
  lock_init();
  if (!g_mounted) return;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return;
  esp_err_t err = esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
  if (err == ESP_OK) {
    g_card = nullptr;
    g_mounted = false;
    ESP_LOGW(TAG, "unmounted: the card will be mounted again when it answers");
  }
  xSemaphoreGive(g_lock);
}

}  // namespace hgp::sd_mount
