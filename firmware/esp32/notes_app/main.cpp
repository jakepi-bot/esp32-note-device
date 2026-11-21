// The notes-only app's entry point (docs/notes-only-firmware-plan.md, section
// 3, option B). Built by the PlatformIO env `notes` (HG_APP=notes), which
// compiles main.cpp out and links this instead.
//
// It carries the hardware and nothing above it: the board table, the e-paper
// panel, the card, the codec, the console, the clock and the radio. There is no
// hg::App here, no core UI, no i18n, no faces, no Opus and no WebSocket, so no
// symbol from any of those is linked. What it does carry from the core is the
// canvas and the font the screens draw through (hg::Canvas in ui_render.cpp),
// which is the renderer's, not the Hermes UI's.
//
// What this file is: the loop. It owns the board's pieces and the two keys; the
// non-UI work (recording, the queue, the radio's duty cycle, power) is
// notes_app/runtime.cpp, and the screens (notes_app/ui_app.*, ui_state.*, drawn
// by ui_render.cpp over ui_device.cpp's ports) answer the keys.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "device_config.hpp"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "notes/sd_mount.hpp"
#include "runtime.hpp"
#include "notes_app/ui_keys.hpp"
#include "port_display_epd.hpp"
#include "ui_device.hpp"
#include "sdkconfig.h"

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
// the note service, and the same stack carries the pair ask.
hgp::PhoneLink g_radio;
hgp::NoteLink g_notelink;
// The station and the phone's setup page (port_wifi.cpp, port_wifi_setup.cpp).
hgp::Wifi g_net;
hgp::DeviceConfig g_config;
hgn::Runtime g_runtime;
// The screens own the store they browse and delete from. It is path logic only
// (notes/notes_store.hpp), so a second instance over the same root as the
// runtime's is two windows on one card, not a second card.
hgp::notes::Store g_store;
// The screens: the key map, the screen machine, the copy, the panel, and the
// note player. The runtime keeps what the screens cannot see - the upload, the
// radio's windows, the retention rule, the shutdown.
hgp::NotesUi g_ui;

// BOOT's two jobs and PWR's. A PWR hold hands over before the latch's own
// hardware cutoff (2.5 s): the flush needs the time, and the release is
// immediate once the card is quiet.
constexpr uint32_t kBootHoldMs = 2000;
constexpr uint32_t kPowerHoldMs = 1500;

enum class Link : uint8_t { Closed, Up, Down };
Link g_link = Link::Closed;  // the station's window: closed, associated, joining

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

// What the screens cannot do themselves goes to the runtime: it owns the
// recording, the sync window and the shutdown. Anything it declines (playback,
// delete) the screens perform on their own ports.
bool run_effect(hgp::notesui::Effect effect, uint16_t note) {
  using hgp::notesui::Action;
  using hgp::notesui::Effect;
  switch (effect) {
    case Effect::BeginRecording:
      return !g_runtime.on_action(static_cast<int>(Action::StartRecording)).empty();
    case Effect::EndRecording:
      return !g_runtime.on_action(static_cast<int>(Action::StopKeep)).empty();
    case Effect::DiscardRecording:
      return !g_runtime.on_action(static_cast<int>(Action::Discard)).empty();
    case Effect::SyncNow:
      g_runtime.sync_now(g_system.now_ms());
      return true;
    case Effect::PowerOff:
      g_runtime.shutdown();
      return true;
    case Effect::None:
    case Effect::StartPlayback:
    case Effect::StopPlayback:
    case Effect::OpenWifiSetup:
    case Effect::OpenPhoneWindow:
    case Effect::PairingAllow:
    case Effect::PairingRefuse:
    case Effect::DeleteNote:
      (void)note;
      return false;  // the screens' own: playback, the ports' windows, the card
  }
  return false;
}

// One key's edge, both jobs: a press under the hold time is a tap, a press that
// reaches it is the hold. `key_since` is 0 when the key is up.
struct KeyPoll {
  bool down = false;
  uint32_t since = 0;
};
KeyPoll g_pwr, g_boot;

template <typename OnTap, typename OnHold>
void poll_key(int pin, KeyPoll& k, uint32_t now, uint32_t hold_ms, OnTap tap, OnHold hold) {
  if (pin < 0) return;
  const bool pressed = gpio_get_level(static_cast<gpio_num_t>(pin)) == 0;  // both keys are active low
  if (pressed && !k.down) {
    k.down = true;
    k.since = now;
    return;
  }
  if (pressed || !k.down) return;
  k.down = false;
  if (now - k.since >= hold_ms) hold(now - k.since);
  else tap();
}

void poll_keys(uint32_t now) {
  const auto& board = hgp::board_config();
  poll_key(board.latch_power.key, g_pwr, now, kPowerHoldMs,
           // A PWR tap is the screen's PowerOff, except inside the menu (select)
           // and on the pairing ask (refuse): ui_keys decides, and the effect
           // takes it from there.
           [now] { g_ui.press(hgp::notesui::Key::Power); },
           [](uint32_t held) {
             // The hold is the graceful path, whatever the screen: flush the
             // card, close the radio, drop the latch. shutdown() does all three,
             // and the release is a no-op on a latch that is already open.
             ESP_LOGW(TAG, "PWR held %u ms: powering off", static_cast<unsigned>(held));
             g_runtime.shutdown();
           });
  poll_key(board.buttons.record, g_boot, now, kBootHoldMs,
           [now] { g_ui.press(hgp::notesui::Key::BootTap); },
           [now](uint32_t held) {
             ESP_LOGI(TAG, "BOOT held %u ms", static_cast<unsigned>(held));
             g_ui.press(hgp::notesui::Key::BootHold);
           });
  (void)now;
}

// The station's window: the runtime says whether this pass needs the network,
// and the port is started and stopped around it. Outside a window the device
// holds no association at all (plan section 4).
void poll_network(uint32_t now) {
  const bool want = g_runtime.wants_network(now);
  if (want && g_link == Link::Closed) {
    g_net.radio_on();
    g_link = Link::Down;
  } else if (!want && g_link != Link::Closed) {
    if (g_net.setup_open()) return;  // a phone is on the setup page: its window
    g_net.radio_off();
    g_link = Link::Closed;
  }
  if (g_link == Link::Closed) {
    g_time.set_network(false);
    return;
  }
  g_net.tick(now);
  const bool up = !g_net.current().empty();
  if (up != (g_link == Link::Up)) {
    g_link = up ? Link::Up : Link::Down;
    // SNTP runs only while the association does: no link, no time traffic.
    g_time.set_network(up);
  }
}

// The phone asks and the transfer's state arrive as events (port_ble.cpp).
void on_phone(const hgp::PhoneSample& sample) {
  using Kind = hgp::PhoneSample::Kind;
  switch (sample.kind) {
    case Kind::Ask:
      // The ask owns the glass until a key answers it: BOOT allows, PWR refuses.
      g_ui.on_pairing_ask();
      ESP_LOGW(TAG, "a phone asks to pair: BOOT allows, PWR refuses");
      break;
    case Kind::Passkey:
      // The code goes on the glass: the user compares it with the phone's.
      ESP_LOGI(TAG, "pairing passkey %06u", static_cast<unsigned>(sample.passkey));
      g_ui.on_pairing_passkey(sample.passkey);
      break;
    case Kind::Paired:
      ESP_LOGI(TAG, "phone paired");
      break;
    case Kind::Failed:
      ESP_LOGW(TAG, "pairing failed");
      break;
    case Kind::Status:
      ESP_LOGI(TAG, "phone: %u paired, %s", static_cast<unsigned>(sample.paired),
               sample.connected ? "one connected" : "none connected");
      break;
  }
}

// The settings the phone's setup page posts ("set server …") and the console's
// own `set`. The notes app keeps the few keys it reads; the rest are answered
// and dropped rather than stored behind a module that is not in this image.
std::string set_line(std::string_view line) {
  const size_t sp = line.find(' ');
  if (sp == std::string_view::npos) return "@error usage: set <key> <value>";
  const std::string key(line.substr(0, sp));
  std::string value(line.substr(sp + 1));
  if (key == "tailnet" || key == "ts_key") return "@ok ignored: this build has no tailnet";
  if (key != "server" && key != "token" && key != "name" && key != "notes_url" && key != "timezone")
    return "@error unknown setting " + key;
  if (!g_storage.set(key, value)) return "@error cannot store " + key;
  if (key == "timezone") g_time.apply_timezone(g_storage);
  ESP_LOGI(TAG, "setting %s stored", key.c_str());
  return "@ok " + key;
}

std::string console_line(std::string_view line) {
  if (line.empty()) return {};
  const auto& board = hgp::board_config();
  if (line == "about") {
    char out[200];
    std::snprintf(out, sizeof out, "notes app %s on %s, sd %s, radio %s, wifi %s", esp_app_get_description()->version,
                  board.name, hgp::sd_mount::mounted() ? "mounted" : "none",
                  g_radio.radio_up() ? g_radio.radio_reason() : "down",
                  g_net.radio_running() ? "on" : "off");
    return out;
  }
  if (line == "sd") {
    uint64_t total = 0, free_bytes = 0;
    if (!hgp::sd_mount::space(&total, &free_bytes)) return std::string("@error no card");
    char out[96];
    std::snprintf(out, sizeof out, "sd %.1f GB free of %.1f GB", static_cast<double>(free_bytes) / 1e9,
                  static_cast<double>(total) / 1e9);
    return out;
  }
  if (line == "keys" || line.rfind("ui", 0) == 0) {
    // `ui` is the screens' own report: the screen, the counts, the card and the
    // link. `ui redraw` forces the full-refresh hand-over `screen` can measure.
    const std::string answer = g_ui.console(line == "keys" ? "ui" : line);
    if (!answer.empty()) return answer;
  }
  if (line == "radio") {
    char out[160];
    std::snprintf(out, sizeof out, "@radio ble %s (%s), wifi %s (%s)", g_radio.radio_up() ? "up" : "down",
                  g_radio.radio_reason(), g_net.radio_running() ? "on" : "off", g_runtime.net_reason());
    return out;
  }
  if (line == "wifi-setup" || line.rfind("wifi-setup ", 0) == 0) {
    // The menu's row on the glass, for a host with no finger on the board: the
    // phone's setup page is the only way to give a device a network when none is
    // saved, and QA has to be able to reach it without the keys.
    if (line.size() > 11) {
      g_net.stop_setup();
      return std::string("@ok setup page closed");
    }
    if (g_net.setup_open()) return std::string("@error the setup page is already open");
    const std::string answer = g_net.start_setup();
    return answer.empty() ? std::string("@error the setup page did not start (see log)") : "@ok " + answer;
  }
  if (line == "phone pair") return g_radio.open_pairing() ? "@ok pairing open, two minutes" : "@error no radio";
  // The two answers the pairing ask takes from a key (BOOT allows, PWR refuses),
  // for a host with no finger on the board: the QA path and the desk bench.
  if (line == "phone allow") {
    g_radio.answer_pairing(true);
    return "@ok ask allowed";
  }
  if (line == "phone refuse") {
    g_radio.answer_pairing(false);
    return "@ok ask refused";
  }
  if (line == "phone wake") {
    g_radio.wake();
    return "@ok window open";
  }
  if (line.rfind("notes", 0) == 0 && (line.size() == 5 || line[5] == ' ')) {
    return g_runtime.console(line.size() > 6 ? line.substr(6) : std::string_view());
  }
  if (line == "battery" || line == "wifi" || line == "clock") return g_runtime.console(line);
  if (line.rfind("set ", 0) == 0) return set_line(line.substr(4));
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
  return std::string("@error unknown command; about, battery, clock, keys, menu, notes, phone, radio, screen, sd, set, wifi");
}

// Console lines arrive as events, answered on this task.
void answer_console(hgp::Event& ev) {
  if (ev.console == nullptr) return;
  const char* text = reinterpret_cast<const char*>(ev.data);
  ev.console->answer(console_line(std::string_view(text ? text : "", ev.len)));
}

void dispatch(hgp::Event& ev) {
  switch (ev.type) {
    case hgp::EventType::Console:
      answer_console(ev);
      break;
    case hgp::EventType::Mic:
      // 20 ms of PCM16 mono: the runtime's recording takes it, or drops it.
      g_runtime.on_mic(reinterpret_cast<const int16_t*>(ev.data), ev.len / sizeof(int16_t));
      break;
    case hgp::EventType::TimeSynced:
      g_runtime.on_clock_synced();
      break;
    case hgp::EventType::Phone:
      if (ev.data && ev.len == sizeof(hgp::PhoneSample)) on_phone(*reinterpret_cast<const hgp::PhoneSample*>(ev.data));
      break;
    default:
      break;  // link events: the station's state is read from the port, not carried
  }
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
  // hold on PWR releases it in hardware, so the shutdown below is the graceful
  // path and comes first (flush, close, drop the latch).
  const bool latched = board.latch_power.enabled && g_latch.begin(board.latch_power);
  if (board.latch_power.enabled && !latched) ESP_LOGE(TAG, "battery latch did not answer");

  const i2c_master_bus_handle_t i2c_bus = hgp::i2c::bus(board.i2c);

  if (board.epd.enabled && g_epd.begin(board.epd)) {
    ESP_LOGI(TAG, "e-paper %s, %ux%u", g_epd.controller_name(), board.epd.width, board.epd.height);
  } else {
    ESP_LOGE(TAG, "the panel did not start: the screens have nothing to draw on");
  }

  if (board.codec.enabled && g_codec.begin(board.codec, i2c_bus)) {
    const bool mic = g_codec_mic.begin(g_codec.in(), board.codec.rmnm_mics, g_codec.mic_link());
    g_codec_speaker.es8311_fade_ramp = board.codec.dac_fade_ramp && board.codec.speaker == hgp::SpeakerCodec::Es8311;
    const bool speaker = g_codec_speaker.begin(g_codec.out(), board.codec.stereo32, board.codec.pa);
    ESP_LOGI(TAG, "codec %s: mic %s, speaker %s", g_codec.mic_name(), mic ? "up" : "none", speaker ? "up" : "none");
  } else {
    ESP_LOGW(TAG, "no codec: recording and playback stay off");
  }

  // The card: configure() before the first ensure(), and a failure is not
  // cached, so a card pushed in later still mounts (notes/sd_mount.hpp).
  hgp::sd_mount::configure(board.sd);
  std::string sd_error;
  if (hgp::sd_mount::ensure(false, &sd_error)) {
    uint64_t total = 0, free_bytes = 0;
    ESP_LOGI(TAG, "card mounted%s", hgp::sd_mount::space(&total, &free_bytes) ? "" : " (the volume did not report its size)");
    if (total) {
      ESP_LOGI(TAG, "card %.1f GB free of %.1f GB", static_cast<double>(free_bytes) / 1e9,
               static_cast<double>(total) / 1e9);
    }
  } else {
    ESP_LOGW(TAG, "no card: %s", sd_error.empty() ? "the slot did not answer" : sd_error.c_str());
  }

  g_config.begin(g_storage);
  // The clock wants network time: this board has no RTC, so every note recorded
  // before the first sync is stamped 1970 (the uploader restamps it later).
  g_time.begin(board.rtc, i2c_bus, g_storage);
  g_time.set_wanted(true);

  // The station comes up stopped: the runtime's first window decides whether it
  // associates at all (docs/notes-only-firmware-plan.md §4).
  g_net.begin(g_storage);
  g_net.on_battery = [] { return g_latch.external_power() == std::optional<bool>(false); };
  g_net.on_console = [](std::string_view line) { return console_line(line); };
  g_net.in_turn = [] { return false; };  // nothing here makes a live turn
  g_net.dozing = [] { return false; };
  g_net.radio_off();

  if (board.bluetooth && g_radio.begin(device_name())) {
    ESP_LOGI(TAG, "radio up as %s", device_name().c_str());
    // The boot window lets a phone that is looking find a new device, and the
    // ask it sends is what the glass shows (plan §4.2). The passkey is the
    // boundary: nothing pairs without a key press on this device.
    g_radio.open_pairing();
  } else {
    ESP_LOGW(TAG, "the radio did not start: the phone path is off");
  }

  hgn::Runtime::Io io;
  io.mic = &g_codec_mic;
  io.power = &g_latch;
  io.ble = &g_notelink;
  io.wifi = &g_net;
  io.time = &g_time;
  io.config = &g_config;
  io.storage = &g_storage;
  io.sd = board.sd;
  io.battery_curve = board.battery_curve;
  io.board_name = board.name;
  if (!g_runtime.begin(io)) {
    ESP_LOGE(TAG, "the runtime did not start: the device can neither record nor upload");
  }

  // The screens take the panel: one full frame of Home, built from the card the
  // runtime just mounted. The recording, the sync window and the shutdown stay
  // the runtime's, which is what run_effect() hands it.
  hgp::NotesUi::Ports ui_ports;
  ui_ports.display = &g_epd;
  ui_ports.speaker = &g_codec_speaker;
  ui_ports.power = &g_latch;
  ui_ports.radio = &g_radio;
  ui_ports.wifi = &g_net;
  ui_ports.time = &g_time;
  ui_ports.system = &g_system;
  ui_ports.storage = &g_storage;
  ui_ports.config = &g_config;
  ui_ports.store = &g_store;
  ui_ports.firmware = esp_app_get_description()->version;
  g_ui.set_effect_hook(run_effect);
  g_ui.begin(ui_ports);

  // The runtime's other half reaches the glass through these (QA D1): a note
  // committed after power-on, a delete, an upload step and a lost card all
  // publish into the screens' snapshot instead of waiting for the next reboot.
  // Set after begin(), so the first frame is the screens' own read of the card.
  hgn::Runtime::UiHooks ui_hooks;
  ui_hooks.changed = [] { g_ui.on_runtime_change(); };
  ui_hooks.upload = [](bool active, uint8_t done, uint8_t total) { g_ui.on_upload(active, done, total); };
  ui_hooks.upload_failed = [] { g_ui.on_upload_failed(); };
  g_runtime.set_ui_hooks(ui_hooks);

  hgp::console::begin();
  ESP_LOGI(TAG, "notes app ready");

  for (;;) {
    hgp::Event ev;
    // A resting notes device has nothing to animate: the panel is bistable and
    // the work arrives as an event (a key, a console line, a phone, the clock).
    // A key's edge ends the wait at once, so the poll below runs at once too.
    if (hgp::events::receive(ev, pdMS_TO_TICKS(200))) {
      do {
        dispatch(ev);
        hgp::events::release(ev);
      } while (hgp::events::receive(ev, 0));
    }
    const uint32_t now = g_system.now_ms();
    g_radio.tick(now);  // the Bluetooth window: up inside it, off at rest
    poll_network(now);
    g_runtime.tick(now, g_link == Link::Up);
    poll_keys(now);
    // The screens' heartbeat: the recording counter, the playback position and
    // the panel's own redraw request. Nothing here runs on a timer of its own.
    g_ui.tick(now);
  }
}
