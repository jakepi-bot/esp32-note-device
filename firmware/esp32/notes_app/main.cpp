// The notes-only app's entry point (docs/notes-only-firmware-plan.md, section
// 3, option B). Built by the PlatformIO env `notes` (HG_APP=notes), which
// compiles main.cpp out and links this instead.
//
// It carries the hardware and nothing above it: the board table, the e-paper
// panel, the card, the codec, the console, the clock and the radio. There is no
// hg::App here, no core UI, no i18n, no faces, no Opus and no WebSocket, so the
// notes image holds no symbol from any of them.
//
// What boots here is deliberately the floor the next two modules stand on:
//   * the panel shows a frame, which proves the SPI panel and its rail;
//   * the card mounts, or says why it did not;
//   * the radio starts (port_ble.cpp), so a phone can find the device;
//   * the serial console answers `screen`, `sd`, `keys` and `about`.
// The screens (notes_app/ui_keys.*) answer the keys with their two-key map, and
// that is logged: this is the skeleton, not the product. Recording, uploading,
// the screens themselves and the power schedule arrive on top of it.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "notes/sd_mount.hpp"
#include "notes_app/ui_keys.hpp"
#include "port_display_epd.hpp"

namespace {

const char* TAG = "hg.notes";

// One instance per peripheral, the way main.cpp holds them: the ports are
// stateful and reachable from their own tasks.
hgp::EspSystem g_system;
hgp::NvsStorage g_storage;
hgp::EpdDisplay g_epd;
hgp::CodecAudio g_codec;
hgp::CodecMic g_codec_mic;
hgp::CodecSpeaker g_codec_speaker;
hgp::LatchPower g_latch;
hgp::TimePort g_time;
// The Bluetooth stack and its services (port_ble.cpp). The notes app advertises
// the note service; the phone console service is dropped in the radio module.
hgp::PhoneLink g_radio;

hgp::notesui::State g_screen;  // the keys' state machine, drawn by the screens

std::string device_name() {
  auto stored = g_storage.get("name");
  return stored && !stored->empty() ? *stored : std::string(CONFIG_HG_DEFAULT_NAME);
}

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// Supplies switched straight from a GPIO go on before anything talks to the
// parts behind them (the e-paper rail and the audio rail on this board).
void power_rails_on(const hgp::BoardConfig& b) {
  bool any = false;
  for (const hgp::PowerRail& rail : b.power_rails) {
    if (rail.gpio < 0) continue;
    const auto pin = static_cast<gpio_num_t>(rail.gpio);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, rail.on_level ? 1 : 0);
    any = true;
  }
  // One tick more than the rounded-down count, so the wait is never shorter.
  if (any && b.power_rails_settle_ms) vTaskDelay(pdMS_TO_TICKS(b.power_rails_settle_ms) + 1);
}

// A frame with no font in it: white field, a two-pixel border. It is the floor
// the screens are drawn on, and it proves the panel, its rail and the partial
// refresh module without pulling the core's canvas and fonts into this image.
void draw_boot_face() {
  uint16_t* fb = g_epd.framebuffer();
  if (fb == nullptr) return;
  const hg::DisplayInfo info = g_epd.info();
  const uint16_t w = info.width, h = info.height;
  if (w == 0 || h == 0) return;
  for (uint32_t i = 0; i < static_cast<uint32_t>(w) * h; ++i) fb[i] = 0xFFFF;
  for (uint16_t y = 0; y < h; ++y) {
    const bool edge = y < 2 || y >= h - 2;
    for (uint16_t x = 0; x < w; ++x) {
      if (edge || x < 2 || x >= w - 2) fb[y * w + x] = 0x0000;
    }
  }
  g_epd.flush(0, h);
}

const char* action_name(hgp::notesui::Action a) {
  using hgp::notesui::Action;
  switch (a) {
    case Action::None: return "none";
    case Action::PowerOff: return "power-off";
    case Action::StartRecording: return "start-recording";
    case Action::StopKeep: return "stop-keep";
    case Action::Discard: return "discard";
    case Action::OpenMenu: return "open-menu";
    case Action::MenuNext: return "menu-next";
    case Action::MenuSelect: return "menu-select";
    case Action::NextNote: return "next-note";
    case Action::OpenNote: return "open-note";
    case Action::NextNoteAction: return "next-note-action";
    case Action::RunNoteAction: return "run-note-action";
    case Action::AskDelete: return "ask-delete";
    case Action::DeleteNote: return "delete-note";
    case Action::KeepNote: return "keep-note";
    case Action::Play: return "play";
    case Action::StopPlay: return "stop-play";
    case Action::AllowPhone: return "allow-phone";
    case Action::RefusePhone: return "refuse-phone";
    case Action::Back: return "back";
  }
  return "?";
}

const char* screen_name(hgp::notesui::Screen s) {
  using hgp::notesui::Screen;
  switch (s) {
    case Screen::Home: return "home";
    case Screen::Recording: return "recording";
    case Screen::Menu: return "menu";
    case Screen::NoteList: return "notes";
    case Screen::NoteAction: return "note";
    case Screen::ConfirmDelete: return "confirm-delete";
    case Screen::Playing: return "playing";
    case Screen::Pairing: return "pairing";
    case Screen::Back: return "back";
  }
  return "?";
}

// BOOT's two jobs and PWR's: the press map is ui_keys.cpp's, so the skeleton
// already answers keys with the rules the screens are written against. Stage 1
// of the plan (PWR powers off, BOOT taps, BOOT holds) is exactly this.
constexpr uint32_t kHoldMs = 2000;

void poll_keys(uint32_t now_ms) {
  const auto& board = hgp::board_config();
  static bool down = false;
  static uint32_t since = 0;
  const int pin = board.buttons.record;
  if (pin < 0) return;
  const bool pressed = gpio_get_level(static_cast<gpio_num_t>(pin)) == 0;  // active low
  if (pressed && !down) {
    down = true;
    since = now_ms;
    return;
  }
  if (pressed || !down) return;
  down = false;
  const uint32_t held = now_ms - since;
  const hgp::notesui::Action action =
      held >= kHoldMs ? hgp::notesui::hold_boot(g_screen) : hgp::notesui::press_boot(g_screen);
  ESP_LOGI(TAG, "BOOT %s (%u ms) -> %s, screen %s", held >= kHoldMs ? "hold" : "tap",
           static_cast<unsigned>(held), action_name(action), screen_name(g_screen.screen));
}

std::string console_line(std::string_view line) {
  if (line.empty()) return {};
  const auto& board = hgp::board_config();
  if (line == "about") {
    char out[160];
    std::snprintf(out, sizeof out, "notes app %s on %s, sd %s, radio %s",
                  esp_app_get_description()->version, board.name,
                  hgp::sd_mount::mounted() ? "mounted" : "none",
                  g_radio.radio_up() ? "up" : "down");
    return out;
  }
  if (line == "sd") {
    uint64_t total = 0, free_bytes = 0;
    if (!hgp::sd_mount::space(&total, &free_bytes)) return std::string("@error no card");
    char out[96];
    std::snprintf(out, sizeof out, "sd %.1f GB free of %.1f GB",
                  static_cast<double>(free_bytes) / 1e9, static_cast<double>(total) / 1e9);
    return out;
  }
  if (line == "keys") {
    std::string out = std::string("screen ") + screen_name(g_screen.screen) + ", menu " +
                      hgp::notesui::menu_label(static_cast<hgp::notesui::MenuItem>(g_screen.menu));
    return out;
  }
  if (line == "menu") {
    std::string out;
    for (uint8_t i = 0; i < hgp::notesui::kMenuCount; ++i) {
      if (i) out += " | ";
      out += i == g_screen.menu ? "[" : "";
      out += hgp::notesui::menu_label(static_cast<hgp::notesui::MenuItem>(i));
      out += i == g_screen.menu ? "]" : "";
    }
    return out;
  }
  // `screen` proves what the panel is showing without a camera pointed at it;
  // one console write takes about 4,000 characters and the image is 5,000, so
  // the slice form is how a host walks it.
  if (line == "screen") return g_epd.image_hex();
  if (line.rfind("screen ", 0) == 0) {
    unsigned first = 0, count = 0;
    const std::string args(line.substr(7));
    if (std::sscanf(args.c_str(), "%u %u", &first, &count) != 2) {
      return std::string("@error usage: screen <first byte> <count> (0 = to the end)");
    }
    return g_epd.image_slice(first, count);
  }
  return std::string("@error unknown command; screen, sd, keys, menu, about");
}

// Console lines arrive as events, answered on this task the way hg::App's
// console() is on the Hermes build.
void answer_console(hgp::Event& ev) {
  if (ev.console == nullptr) return;
  const char* text = reinterpret_cast<const char*>(ev.data);
  ev.console->answer(console_line(std::string_view(text ? text : "", ev.len)));
}

}  // namespace

extern "C" void app_main(void) {
  hgp::diag::begin();  // first, so `diag log` has the whole boot
  init_nvs();
  hgp::events::init();
  ESP_ERROR_CHECK(g_storage.begin() ? ESP_OK : ESP_FAIL);

  static hgp::BoardConfig board_store;
  board_store = hgp::board_config();
  const hgp::BoardConfig& board = board_store;

  power_rails_on(board);
  // The battery latch holds the supply while the board is on battery; a 2.5 s
  // hold on PWR releases it in hardware, so power_off() here is the graceful
  // path (flush, close, drop the latch).
  const bool latched = board.latch_power.enabled && g_latch.begin(board.latch_power);
  if (board.latch_power.enabled && !latched) ESP_LOGE(TAG, "battery latch did not answer");

  const i2c_master_bus_handle_t i2c_bus = hgp::i2c::bus(board.i2c);

  if (board.epd.enabled && g_epd.begin(board.epd)) {
    draw_boot_face();
    ESP_LOGI(TAG, "e-paper %s, %ux%u", g_epd.controller_name(), board.epd.width, board.epd.height);
  } else {
    ESP_LOGE(TAG, "the panel did not start: the screens have nothing to draw on");
  }

  if (board.codec.enabled && g_codec.begin(board.codec, i2c_bus)) {
    const bool mic = g_codec_mic.begin(g_codec.in(), board.codec.rmnm_mics, g_codec.mic_link());
    g_codec_speaker.es8311_fade_ramp =
        board.codec.dac_fade_ramp && board.codec.speaker == hgp::SpeakerCodec::Es8311;
    const bool speaker = g_codec_speaker.begin(g_codec.out(), board.codec.stereo32,
                                               board.codec.pa);
    ESP_LOGI(TAG, "codec %s: mic %s, speaker %s", g_codec.mic_name(), mic ? "up" : "none",
             speaker ? "up" : "none");
  } else {
    ESP_LOGW(TAG, "no codec: recording and playback stay off");
  }

  // The card: configure() before the first ensure(), and a failure is not
  // cached, so a card pushed in later still mounts (notes/sd_mount.hpp).
  hgp::sd_mount::configure(board.sd);
  std::string sd_error;
  if (hgp::sd_mount::ensure(false, &sd_error)) {
    uint64_t total = 0, free_bytes = 0;
    ESP_LOGI(TAG, "card mounted%s", hgp::sd_mount::space(&total, &free_bytes)
                                        ? "" : " (the volume did not report its size)");
    if (total) {
      ESP_LOGI(TAG, "card %.1f GB free of %.1f GB", static_cast<double>(free_bytes) / 1e9,
               static_cast<double>(total) / 1e9);
    }
  } else {
    ESP_LOGW(TAG, "no card: %s", sd_error.empty() ? "the slot did not answer" : sd_error.c_str());
  }

  g_time.begin(board.rtc, i2c_bus, g_storage);

  if (board.bluetooth && g_radio.begin(device_name())) {
    ESP_LOGI(TAG, "radio up as %s", device_name().c_str());
  } else {
    ESP_LOGW(TAG, "the radio did not start: the phone path is off");
  }

  hgp::console::begin();
  ESP_LOGI(TAG, "notes app ready");

  for (;;) {
    hgp::Event ev;
    // A resting notes device has nothing to animate: the panel is bistable and
    // the work arrives as an event (a key, a console line, a phone).
    if (hgp::events::receive(ev, pdMS_TO_TICKS(200))) {
      do {
        if (ev.type == hgp::EventType::Console) answer_console(ev);
        hgp::events::release(ev);
      } while (hgp::events::receive(ev, 0));
    }
    const uint32_t now = g_system.now_ms();
    g_radio.tick(now);  // the radio's window: up inside it, off at rest
    if (latched && g_latch.take_key_press()) {
      ESP_LOGW(TAG, "PWR held: powering off");
      g_latch.power_off();
    }
    poll_keys(now);
  }
}
