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
#include "key_poll.hpp"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
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
// Nothing has been touched for this long, on the cell, with no work in flight:
// the device goes idle - the panel's rail drops and every render is refused until
// a key or a console line comes back. Sixty seconds is past the point where a
// person is still reading the screen (2026-11-24: 6 hours idle on a cell).
constexpr uint32_t kIdleEnterMs = 60000;
// The loop's wait while idle: the key interrupt wakes it, so this only sets how
// often the timers check in, and ten seconds is cheap against a 2 s heartbeat.
constexpr uint32_t kIdleWaitMs = 10000;
constexpr uint32_t kPowerHoldMs = 1500;
// A power source that changes under the firmware must not read as a finger on
// PWR for this long: the sense line is part of the power circuit on this board.
constexpr uint32_t kKeyLockMs = 2000;

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
    case Effect::BeginRecording: {
      const std::string answer = g_runtime.on_action(static_cast<int>(Action::StartRecording));
      // Runtime::start_note logs its own reason on every refusal, so this is
      // the summary line: the glass goes back to Home and the log says why.
      if (answer.empty()) ESP_LOGW(TAG, "the runtime refused to start a recording");
      return !answer.empty();
    }
    case Effect::EndRecording: {
      const std::string answer = g_runtime.on_action(static_cast<int>(Action::StopKeep));
      if (answer.empty()) ESP_LOGW(TAG, "the runtime was not recording: nothing to commit");
      return !answer.empty();
    }
    case Effect::DiscardRecording:
      return !g_runtime.on_action(static_cast<int>(Action::Discard)).empty();
    case Effect::SyncNow:
      g_runtime.sync_now(g_system.now_ms());
      return true;
    case Effect::WipeNotes: {
      // The menu's Wipe notes, after its confirm: the runtime checks the guard
      // and starts the wipe on its own task (t_0070a731), so this returns in
      // milliseconds and the glass draws its busy frame from the effect.
      const std::string answer = g_runtime.on_action(static_cast<int>(Action::WipeNotes));
      if (answer.empty()) ESP_LOGW(TAG, "the runtime refused the wipe: its reason is above");
      return !answer.empty();
    }
    case Effect::FormatCard: {
      const std::string answer = g_runtime.on_action(static_cast<int>(Action::FormatCard));
      if (answer.empty()) ESP_LOGW(TAG, "the runtime refused the format: its reason is above");
      return !answer.empty();
    }
    case Effect::PowerOff:
      // The screens' power-off from Home. The sleeping face goes on the panel
      // first: this is the last frame the device draws, and the e-paper keeps
      // it, so an off gadget has to say so (t_41477fd1). It has to land before
      // shutdown(): that closes the radio and unmounts the card, and the full
      // refresh needs the panel driver and the time the latch leaves.
      g_ui.sleep_face();
      g_runtime.shutdown();
      return true;
    case Effect::None:
    case Effect::StartPlayback:
    case Effect::StopPlayback:
    case Effect::OpenWifiSetup:
    case Effect::PairingAllow:
    case Effect::PairingRefuse:
    case Effect::DeleteNote:
    case Effect::UnpairPeer:
      (void)note;
      return false;  // the screens' own: playback, the ports' windows, the card, the bond store
    case Effect::OpenPhoneWindow:
    case Effect::ClosePhoneWindow:
      // The screens' own too, and the one this bench could not see: the runtime
      // declined the Phone window while the UI pushed it at a second, different
      // call, so a window that never opened left no line anywhere. It is logged
      // here and answered by port_ble, which is the only place that can say why
      // (t_9972e2a1).
      (void)note;
      ESP_LOGI(TAG, "the Phone window is the screens' own: port_ble opens it and says why not");
      return false;
  }
  return false;
}

// One key's edge, both jobs: a press under the hold time is a tap, a press that
// reaches it is the hold. `key_since` is 0 when the key is up.
//
// Two rules keep a key that is not being pressed from firing. A key that is
// already down when the firmware starts looking (or right after the power source
// changed) is ignored until it is let go: on this board the PWR sense line is
// part of the power circuit, and a level that sits low while the board changes
// from cable to battery must not read as a finger. The board then powers down
// 1.5 s later, on a press nobody made, and will not come back on.
// KeyPoll and its timing rules live in key_poll.hpp so the host tests can drive
// them without a chip: the sleep-gap rule is the one that broke.
using hgp::KeyPoll;
KeyPoll g_pwr, g_boot;
// A press the bench can make without a finger: the same poll path a GPIO level
// takes, so the timing rules are exercised for real (`key virtual boot down`).
bool g_virtual_boot = false, g_virtual_pwr = false;
// A press arrived with the screen dark - from the GPIO or from the console's
// `key` verb, which is a press for a bench with no finger on the board. The loop
// consumes it on the next pass, so a scripted press wakes idle exactly like a
// real one.
bool g_key_while_idle = false;
esp_pm_lock_handle_t g_key_awake = nullptr;  // held while a key is down
uint32_t g_keys_ready_ms = 0;  // no press counts before this (boot, power change)
bool g_keys_locked = false;    // a key is being ignored until it is released

// PWR + BOOT: whatever is down now is not a press. Called at boot and when the
// power source changes under a finger that is not there.
void lock_keys(uint32_t now) {
  g_keys_ready_ms = now + kKeyLockMs;
  g_keys_locked = true;
  g_pwr.down = g_boot.down = false;
  g_pwr.armed = g_boot.armed = false;
  g_pwr.ignored = g_boot.ignored = false;
}

template <typename OnTap, typename OnHold>
void poll_key(int pin, KeyPoll& k, uint32_t now, uint32_t hold_ms, bool virtual_down, OnTap tap, OnHold hold) {
  if (pin < 0 && !virtual_down) return;
  const bool pressed = virtual_down || (pin >= 0 && gpio_get_level(static_cast<gpio_num_t>(pin)) == 0);
  // Light sleep is time nobody can hold a key through: the chip is asleep, the
  // clock is not. Take exactly that out of the press, and only that - a loop busy
  // with an upload step is still a finger on the key (Jake, 2026-11-24: a tap
  // opened the menu because the release landed after a doze).
  const uint32_t slept = hgp::EspSystem::slept_ms_total();
  if (slept != k.slept_at) {
    hgp::discount_sleep(k, slept - k.slept_at);
    k.slept_at = slept;
  }
  k.polled_at = now;
  if (!pressed) {
    k.armed = true;
    if (!k.down) return;
    k.down = false;
    if (k.ignored) {  // the tail of a press that never counted
      k.ignored = false;
      return;
    }
    if (now - k.since >= hold_ms) hold(now - k.since);
    else tap();
    return;
  }
  if (k.down) return;
  k.down = true;
  k.since = now;
  if (!k.armed || now < g_keys_ready_ms) {
    // Held through boot or a power change, or the lock is still running: the
    // press is not a finger, and its release must not become a tap either.
    k.ignored = true;
    if (g_keys_locked) {
      ESP_LOGW(TAG, "a key is down but ignored until it is released");
      g_keys_locked = false;
    }
  }
}

// The PWR hold, the one power gesture: the screens own the policy (a hold
// powers off from Home and from nowhere else), the runtime does the shutting
// down, and the sleeping face goes on the glass in between - it is the last
// frame the device draws, and the e-paper keeps it, so an off gadget has to say
// so (t_41477fd1). Order matters: sleep_face() waits for the panel's full
// waveform, and shutdown() then drops the radio, unmounts the card and lets the
// latch go, after which nothing can draw.
void power_hold(uint32_t held) {
  // Flush the card, close the radio and drop the latch before the latch's own
  // 2.5 s cutoff. A hold in a submenu is still a press the hardware will cut at
  // 2.5 s, and a dirty card is not the price of that: the face is drawn and the
  // shutdown run whatever the screens answer.
  ESP_LOGW(TAG, "PWR held %u ms: powering off", static_cast<unsigned>(held));
  const hgp::notesui::Outcome out = g_ui.press(hgp::notesui::Key::PowerHold);
  if (out.effect == hgp::notesui::Effect::PowerOff) return;  // the effect did it
  g_ui.sleep_face();
  g_runtime.shutdown();
}

// A key edge is an event, not something to wait for: without this the loop's
// resting wait (2 s) was also the delay before a press was even sampled, so a tap
// had to be held about a second to be caught at all, and the hold gesture needed
// three (Jake, 2026-11-24). The ISR only queues a KeyEdge; the poll does the rest.
void on_key_edge(void*) { hgp::events::post_from_isr(hgp::EventType::KeyEdge); }

void arm_key_edges() {
  const auto& b = hgp::board_config();
  if (!hgp::gpio_isr_service()) {
    ESP_LOGW(TAG, "no GPIO interrupt service: keys are polled only");
    return;
  }
  for (int pin : {b.buttons.record, b.latch_power.key}) {
    if (pin < 0) continue;
    const auto gpio = static_cast<gpio_num_t>(pin);
    // Keep the input connected through light sleep, or the wake and the edge
    // interrupt both see a disconnected pin.
    gpio_sleep_sel_dis(gpio);
    gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE);
    if (gpio_isr_handler_add(gpio, &on_key_edge, nullptr) != ESP_OK) {
      ESP_LOGW(TAG, "key GPIO %d has no edge interrupt; polling it", pin);
      continue;
    }
    gpio_intr_enable(gpio);
  }
}

// True while a finger is on either key: the loop polls faster and light sleep
// waits, so a press is measured from the moment it was seen.
bool key_down_now() { return g_boot.down || g_pwr.down; }

void poll_keys(uint32_t now) {
  const auto& board = hgp::board_config();
  poll_key(board.latch_power.key, g_pwr, now, kPowerHoldMs, g_virtual_pwr,
           // A PWR tap is never a power-off: ui_keys decides what the screen does
           // with it (open, select, refuse, stop and go back) and the effect
           // takes it from there. The tap used to reach Action::PowerOff from
           // Home, Recording, About and the player, so one press switched the
           // device off (t_dec6f9ef).
           [now] { g_ui.press(hgp::notesui::Key::Power); }, power_hold);
  poll_key(board.buttons.record, g_boot, now, kBootHoldMs, g_virtual_boot,
           [now] { g_ui.press(hgp::notesui::Key::BootTap); },
           [now](uint32_t held) {
             ESP_LOGI(TAG, "BOOT held %u ms", static_cast<unsigned>(held));
             g_ui.press(hgp::notesui::Key::BootHold);
           });
  // Keep the CPU awake for as long as the key is down, and give it back the
  // moment it is released.
  if (g_key_awake != nullptr) {
    static bool held = false;
    const bool down = key_down_now();
    if (down && !held) { esp_pm_lock_acquire(g_key_awake); held = true; }
    else if (!down && held) { esp_pm_lock_release(g_key_awake); held = false; }
  }
  (void)now;
}

// "Off" on a board whose latch cannot cut the supply (a board on USB): deep
// sleep, with the power key as the wake source, so a device put away does not
// flatten its cell. The key is active low; waking reboots into a normal boot.
void sleep_until_power_key() {
  const auto& b = hgp::board_config();
  // EXT0 is the deep-sleep wake source for one pin held low: the PWR key on this
  // board is active low, and GPIO 17 is an RTC pin on the S3.
  if (b.latch_power.key >= 0) {
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(b.latch_power.key), 0);
  }
  esp_deep_sleep_start();
}

// The station's window: the runtime says whether this pass needs the network,
// and the port is started and stopped around it. Outside a window the device
// holds no association at all (plan section 4).
void poll_network(uint32_t now) {
  const bool want = g_runtime.wants_network(now);
  // A start that did not take is not a station that is joining. `g_link` used to
  // be set Down unconditionally after radio_on(), so a refused start (esp_wifi
  // out of memory at boot, a chip that never came back from a failed join) left
  // the state machine convinced the link was on its way: `want` stayed true,
  // `g_link` was never Closed again, radio_on() was never called again, and the
  // station stayed off until a reboot — 11 notes stranded, "wifi on" a no-op
  // (Jake, 2026-11-22). Ask again, on a slow clock so a dead radio cannot spin.
  static uint32_t last_try = 0;
  const bool radio_dead = want && !g_net.radio_running();
  if (want && (g_link == Link::Closed || radio_dead)) {
    if (g_link == Link::Closed || static_cast<int32_t>(now - last_try) > 20000) {
      last_try = now;
      g_net.radio_on();
      // `radio_on()` is the only thing that knows whether the chip started; ask
      // it rather than assuming.
      g_link = g_net.radio_running() ? Link::Down : Link::Closed;
    }
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
      // The asking phone's address goes with it, so the bond that lands later is
      // matched to the question it answers (t_da500270).
      g_ui.on_pairing_ask(sample.peer);
      ESP_LOGW(TAG, "a phone asks to pair: BOOT allows, PWR refuses");
      break;
    case Kind::Passkey:
      // The code goes on the glass: the user compares it with the phone's.
      ESP_LOGI(TAG, "pairing passkey %06u", static_cast<unsigned>(sample.passkey));
      g_ui.on_pairing_passkey(sample.passkey);
      break;
    case Kind::Paired:
      // The bond is made - by a key, or by the console's `phone allow`, which
      // never touches the screen: either way the ask is answered and the glass
      // leaves it without a key.
      ESP_LOGI(TAG, "phone paired");
      g_ui.on_pairing_paired(sample.peer);
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
  // `set wifi on|off` is the phone setup page's spelling, and it is the same
  // switch as the console's `wifi on|off`: the choice is stored in NVS and
  // survives a power cycle.
  if (key == "wifi") {
    if (value != "on" && value != "off") return "@error wifi must be on or off";
    g_runtime.set_wifi_stay(value == "on");
    return std::string("@ok wifi ") + value;
  }
  if (key == "device_key") {
    // The device's identity is derived from this key, and an NVS wipe makes a new
    // one: the hub then answers "device ... is not enrolled" to every note. This
    // puts a known key back (from the hub's devices.json) so the device is itself
    // again. Base64, 32 bytes; a reboot is what loads it (load_identity() runs
    // once, in begin()).
    std::vector<uint8_t> raw;
    if (!hg::crypto::base64_decode(value, raw) || raw.size() != 32)
      return "@error device_key must be base64 of 32 bytes";
    if (!g_storage.set("device_key", value)) return "@error cannot store device_key";
    return "@ok device key stored; reboot to use it";
  }
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
    // link. `ui home` forces the full-refresh hand-over `screen` can measure.
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
  if (line == "phone") {
    // What the pairing table holds: three slots on this build, and a full table
    // refuses every new phone until one is removed.
    const auto window = g_radio.phone_window();
    char out[128];
    std::snprintf(out, sizeof out, "@phone %u of %u paired%s, radio %s", static_cast<unsigned>(window.paired),
                  static_cast<unsigned>(window.slots), window.connected ? ", one connected" : "",
                  g_radio.radio_up() ? "up" : "down");
    return out;
  }
  if (line == "phone forget" || line == "phone clear") {
    // Every saved phone goes: the bond store in NVS is emptied, so the next phone
    // that asks is new again. This is the way out of a full table (3 of 3) on a
    // device whose three phones are gone, lost or replaced.
    g_radio.forget();
    return "@ok every paired phone removed; the table is empty";
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
    char out[96];
    std::snprintf(out, sizeof out, "@ok wake: ble %s (%s)", g_radio.radio_up() ? "up" : "down",
                  g_radio.radio_reason());
    return out;
  }
  if (line.rfind("notes", 0) == 0 && (line.size() == 5 || line[5] == ' ')) {
    return g_runtime.console(line.size() > 6 ? line.substr(6) : std::string_view());
  }
  // `wifi` and its arguments: the runtime owns on/off and the status line. Only
  // the bare verb used to reach it, so `wifi off` - which the usage advertises -
  // answered "unknown command" (2026-11-23).
  if (line == "battery" || line == "clock") return g_runtime.console(line);
  std::string wtrim(line);
  while (!wtrim.empty() && (wtrim.back() == '\r' || wtrim.back() == '\n' || wtrim.back() == ' '))
    wtrim.pop_back();
  // `wifi` is the one verb port_console re-quotes, because a Wi-Fi password may
  // hold spaces: the app sees `wifi "off"`, quotes and all. Unquote before
  // matching, or the one spelling the usage advertises never works (2026-11-23).
  if (wtrim.rfind("wifi ", 0) == 0) {
    std::string arg = wtrim.substr(5);
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') arg = arg.substr(1, arg.size() - 2);
    if (arg == "on" || arg == "off") {
      const bool on = arg == "on";
      g_runtime.set_wifi_stay(on);
      return std::string("@ok wifi ") + (on ? "on: the station stays associated while plugged in"
                                            : "off: windows only");
    }
    return "@error usage: wifi [on | off]";
  }
  if (wtrim == "wifi") return g_runtime.console(wtrim);
  if (line == "pmsleep") {
    // Force one light sleep with a one-second timer: if this works, the hardware
    // and the clocks are fine and something is keeping the idle task off the CPU.
    esp_sleep_enable_timer_wakeup(1000000);
    const int64_t before = static_cast<int64_t>(g_system.now_us());
    const esp_err_t err = esp_light_sleep_start();
    char out[160];
    std::snprintf(out, sizeof out, "@ok light sleep: %s, awake after %lld ms, slept total %u ms",
                  esp_err_to_name(err), (static_cast<int64_t>(g_system.now_us()) - before) / 1000,
                  static_cast<unsigned>(hgp::EspSystem::slept_ms_total()));
    return out;
  }
  if (line == "pm") {
    // Who is keeping the CPU awake. Light sleep that never engages has a holder,
    // and this names it: the battery work needs the holder, not a guess.
    esp_pm_dump_locks(stdout);
    return "@ok power-management locks dumped above";
  }
  if (line == "power") {
    // The three lines that decide whether a board on battery stays up: the
    // latch's pad level, the PWR key's own level, and what the port thinks the
    // supply is. A latch that reads 0 with a charged cell is a power cut the
    // moment the cable leaves; a key that reads 0 while nobody touches it is the
    // phantom press the loop now ignores.
    const auto ext = g_latch.external_power();
    char out[180];
    std::snprintf(out, sizeof out, "@power latch gpio%d=%d (held %s) | pwr key gpio%d=%d (%s) | supply %s", board.latch_power.enable,
                  g_latch.latch_level(), g_latch.hold() ? "yes" : "NO",
                  board.latch_power.key, g_latch.key_level(),
                  g_latch.key_level() == 0 ? "down" : "released",
                  ext.has_value() ? (*ext ? "cable" : "battery") : "unknown");
    return out;
  }
  if (line.rfind("key virtual ", 0) == 0) {
    // A press held down and released by the console, through the poll path a GPIO
    // level takes - including the light sleep in between, which is where a tap
    // used to be measured as a hold and open the menu instead of recording.
    const std::string_view arg(line.substr(12));
    const bool down = arg.size() > 5 && arg.substr(arg.size() - 5) == " down";
    const bool up = arg.size() > 3 && arg.substr(arg.size() - 3) == " up";
    if (!down && !up) return "@error usage: key virtual boot|pwr down|up";
    const bool value = down;
    if (arg.rfind("boot", 0) == 0) g_virtual_boot = value;
    else if (arg.rfind("pwr", 0) == 0) g_virtual_pwr = value;
    else return "@error usage: key virtual boot|pwr down|up";
    return down ? "@ok key down" : "@ok key up";
  }
  if (line.rfind("key ", 0) == 0) {
    // A scripted press counts as a finger, so it also ends an idle the console
    // put the device into (otherwise the state moves and the glass stays dark).
    g_key_while_idle = true;
    // The same press the keys make, for a bench with no finger on the board.
    // `press()` runs the screen machine and the effect synchronously, so the
    // screen this prints is the one the glass was handed, not a guess.
    const std::string_view which(line.substr(4));
    if (which == "boot") {
      g_ui.press(hgp::notesui::Key::BootTap);
      return std::string("@ok boot tap -> screen ") + g_ui.screen_word();
    }
    if (which == "hold") {
      g_ui.press(hgp::notesui::Key::BootHold);
      return std::string("@ok boot hold -> screen ") + g_ui.screen_word();
    }
    if (which == "pwr") {
      g_ui.press(hgp::notesui::Key::Power);
      return std::string("@ok pwr tap");
    }
    return std::string("@error usage: key boot|hold|pwr");
  }
  if (line.rfind("wifi-set ", 0) == 0) {
    // Provision the station from a host at a desk: the same credentials the
    // phone's setup page writes, through the same provision() path that saves
    // them and joins. Without this, a wiped NVS needs a phone just to get back
    // on the network (2026-11-22).
    const std::string args(line.substr(9));
    const size_t sp = args.find(' ');
    if (sp == std::string::npos || sp == 0) return "@error usage: wifi-set <ssid> <password>";
    hg::WifiCredentials cred;
    const std::string ssid = args.substr(0, sp);
    const std::string psk = args.substr(sp + 1);
    if (ssid.size() >= sizeof cred.ssid || psk.size() >= sizeof cred.password)
      return "@error too long for the stored field";
    std::snprintf(cred.ssid, sizeof cred.ssid, "%s", ssid.c_str());
    std::snprintf(cred.password, sizeof cred.password, "%s", psk.c_str());
    g_net.set_credentials(ssid, psk);
    return "@ok credentials stored; joining";
  }
  if (line.rfind("epd ", 0) == 0) {
    // The glass's own knob: `epd full` clears the residue now (a demo wants a
    // clean page), `epd ghost N` trades charge for cleanliness - 1 is a full
    // refresh every frame, 10 the default.
    const std::string_view rest(line.substr(4));
    if (rest.rfind("idle", 0) == 0) {
      // The bench has to see the idle state without unplugging: on the cable the
      // device never idles by design (somebody is at the desk with a meter).
      const bool on = rest != "idle off";
      g_ui.idle(on);
      g_runtime.set_idle(on);
      return on ? "@ok idle: panel rail down, a key wakes it (epd idle off)"
                : "@ok awake: rail up, full redraw";
    }
    if (rest == "wake") {
      g_ui.wake_from_sleep_face();
      return "@ok awake again (full redraw)";
    }
    if (rest == "sleep") {
      // Draw the sleeping face without powering off: the bench has to photograph
      // it, and `notes poweroff` on USB now deep-sleeps until someone presses a
      // key (2026-11-23).
      g_ui.sleep_face();
      return "@ok sleeping face drawn (the device is still on)";
    }
    if (rest == "full") {
      g_epd.request_full_refresh();
      return "@ok the next frame is a full refresh";
    }
    if (rest.rfind("ghost ", 0) == 0) {
      const int n = std::atoi(std::string(rest.substr(6)).c_str());
      g_epd.set_full_every(n);
      char out[64];
      std::snprintf(out, sizeof out, "@ok full refresh every %d partials", g_epd.full_every());
      return out;
    }
    return "@error usage: epd full | epd sleep | epd wake | epd idle [on|off] | epd ghost <1..100>";
  }
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
  return std::string("@error unknown command; about, battery, clock, key, keys, menu, notes, phone, radio, screen, sd, set, wifi [on|off]");
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
  // cached, so a card pushed in later still mounts (notes/sd_mount.hpp). A card
  // that is slow to answer after power-on is retried here, quickly, instead of
  // being left to the runtime's 30 s clock: five attempts over about four
  // seconds at boot, then the runtime's own retry takes over.
  hgp::sd_mount::configure(board.sd);
  std::string sd_error;
  bool sd_ok = false;
  for (int attempt = 0; attempt < 5 && !sd_ok; ++attempt) {
    sd_ok = hgp::sd_mount::ensure(false, &sd_error);
    if (!sd_ok) vTaskDelay(pdMS_TO_TICKS(250));  // the ensure() throttle is one attempt per second
  }
  if (sd_ok) {
    uint64_t total = 0, free_bytes = 0;
    ESP_LOGI(TAG, "card mounted%s", hgp::sd_mount::space(&total, &free_bytes) ? "" : " (the volume did not report its size)");
    if (total) {
      ESP_LOGI(TAG, "card %.1f GB free of %.1f GB", static_cast<double>(free_bytes) / 1e9,
               static_cast<double>(total) / 1e9);
    }
  } else {
    ESP_LOGW(TAG, "no card after five attempts: %s", sd_error.empty() ? "the slot did not answer" : sd_error.c_str());
  }

  g_config.begin(g_storage);
  // Power management: the full gadget app configures it, and the notes build
  // inherited the Kconfig without the call - so the CPU sat at 240 MHz and light
  // sleep never engaged. With the radio off this app idled at 39 mA (meter,
  // 2026-11-23), which is a processor awake for nothing.
  {
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = hgp::board_config().light_sleep;
    if (esp_pm_configure(&pm) != ESP_OK) {
      ESP_LOGW(TAG, "power management unavailable; the CPU stays at full speed");
    } else {
      ESP_LOGI(TAG, "power management: %d-%d MHz, light sleep %s", static_cast<int>(pm.min_freq_mhz),
               static_cast<int>(pm.max_freq_mhz), pm.light_sleep_enable ? "on" : "off");
    }
    // The keys are polled, and a chip in light sleep cannot poll: without a wake
    // source a press waits for the next timer (up to 2 s) and a short tap is lost
    // entirely - "I cannot start a recording" (Jake, 2026-11-24). Both keys are
    // active low, so the low level is the wake.
    const auto& b = hgp::board_config();
    for (int pin : {b.buttons.record, b.latch_power.key}) {
      if (pin >= 0) gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
    }
    if (esp_sleep_enable_gpio_wakeup() != ESP_OK) {
      ESP_LOGW(TAG, "the keys cannot wake light sleep: presses wait for the next timer");
    }
    // And the CPU is kept awake while a finger is on a key, so the release is
    // sampled instead of slept through (a hold would otherwise read as a tap).
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-key", &g_key_awake) != ESP_OK) g_key_awake = nullptr;
    // The key timing subtracts the light sleep this app actually took, so the
    // counter has to be running even though this app never takes the doze lock.
    hgp::EspSystem::enable_sleep_accounting();
    arm_key_edges();
  }
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
  io.sleep_now = &sleep_until_power_key;
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
  // The band's level is the runtime's percentage: one conversion, one curve.
  ui_ports.battery_curve = board.battery_curve;
  ui_ports.firmware = esp_app_get_description()->version;
  g_ui.set_effect_hook(run_effect);
  // The runtime is the truth about the capture: the screens follow its answer,
  // so a recording stopped from the console leaves the recording face, and one
  // started there reaches it.
  g_ui.set_capture_probe({[] { return g_runtime.recording(); }, [] { return g_runtime.record_elapsed_s(); },
                          [] { return g_runtime.recording_queued(); }, [] { return g_runtime.record_cap_s(); },
                          [] { return g_runtime.record_cap_bytes(); },
                          [] { return g_runtime.busy_reason(); }});
  // The wipe and the format run on the runtime's own task, so the glass hears
  // their counts and their end here instead of waiting inside the effect
  // (t_0070a731): a 114-note wipe is ~90 s of card work and a format 22.8 s.
  hgp::NotesUi::CardJobProbe card_job_probe;
  card_job_probe.active = [](bool* format, uint32_t* done, uint32_t* total) {
    const hgn::Runtime::CardJob job = g_runtime.card_job();
    if (!job.active) return false;
    *format = job.format;
    *done = job.done;
    *total = job.total;
    return true;
  };
  card_job_probe.result = [](bool* ok, std::string* why) { return g_runtime.take_card_job_result(ok, why); };
  g_ui.set_card_job_probe(card_job_probe);
  g_ui.begin(ui_ports);

  // The runtime's other half reaches the glass through these (QA D1): a note
  // committed after power-on, a delete, an upload step and a lost card all
  // publish into the screens' snapshot instead of waiting for the next reboot.
  // Set after begin(), so the first frame is the screens' own read of the card.
  hgn::Runtime::UiHooks ui_hooks;
  ui_hooks.changed = [] { g_ui.on_runtime_change(); };
  ui_hooks.upload = [](bool active, uint8_t done, uint8_t total) { g_ui.on_upload(active, done, total); };
  ui_hooks.upload_failed = [] { g_ui.on_upload_failed(); };
  // A stop that found no audio throws the capture away and stays on Home: with
  // the counts unchanged, the line under the hero is the only thing that can say
  // why nothing was saved.
  ui_hooks.discarded = [] { g_ui.on_discarded(); };
  g_runtime.set_ui_hooks(ui_hooks);

  hgp::console::begin();
  ESP_LOGI(TAG, "notes app ready");

  // Nothing on the keys counts for the first couple of seconds: a key held
  // through the boot (or a sense line that is low while the board settles) is
  // not a press, and a phantom one would power the board off on battery.
  lock_keys(g_system.now_ms());

  bool power_known = false, power_cable = false;
  uint32_t last_touch_ms = g_system.now_ms();
  for (;;) {
    hgp::Event ev;
    // A resting notes device has nothing to animate: the panel is bistable and
    // the work arrives as an event (a key, a console line, a phone, the clock).
    // A key's edge ends the wait at once, so the poll below runs at once too.
    // A working device polls five times a second; a resting one, once.
    // 10 ms while a finger is on a key: a 100 ms tap is then three samples and
    // the release is seen at once. A resting device still waits two seconds.
    const uint32_t wait_ms = key_down_now() ? hgp::kKeyPollMs
                            : g_ui.idle()    ? kIdleWaitMs
                            : g_runtime.busy(g_system.now_ms()) ? 200
                                                                : 2000;
    if (hgp::events::receive(ev, pdMS_TO_TICKS(wait_ms))) {
      do {
        // Only a finger ends idle. A console line is somebody at a bench too,
        // but counting it would mean a script polling `ui` keeps the panel powered
        // forever, and the console's own way out is `epd idle off`.
        const bool person = ev.type == hgp::EventType::KeyEdge || ev.type == hgp::EventType::Key;
        dispatch(ev);
        hgp::events::release(ev);
        if (person) {
          last_touch_ms = g_system.now_ms();
          g_key_while_idle = true;
        }
      } while (hgp::events::receive(ev, 0));
    }
    const uint32_t now = g_system.now_ms();
    // The supply changed: plugging or unplugging the cable moves the PWR sense
    // line, and that edge is not a finger. Lock the keys and say so.
    if (const auto cable = g_latch.external_power(); cable.has_value()) {
      if (power_known && *cable != power_cable) {
        ESP_LOGW(TAG, "power source: %s; keys locked for %u ms", *cable ? "cable" : "battery",
                 static_cast<unsigned>(kKeyLockMs));
        lock_keys(now);
      }
      power_known = true;
      power_cable = *cable;
    }
    // The pad is the supply on a battery board: keep it driven high. Cheap, and
    // it only logs when the level was lost.
    g_latch.hold();
    g_radio.tick(now);  // the Bluetooth window: up inside it, off at rest
    poll_network(now);
    g_runtime.tick(now, g_link == Link::Up);
    poll_keys(now);
    // Idle, entered on the cell only - a device on the cable is somebody's bench
    // with a meter on it - and left only for a reason: a key, or work to do. The
    // first version also woke it merely because the cable was present, which
    // cancelled `epd idle on` within one loop pass (frames climbed 4 -> 6 in the
    // 45 s it was supposed to be dark).
    if (!g_ui.idle()) {
      if (!power_cable && !g_runtime.busy(now) && now - last_touch_ms > kIdleEnterMs) {
        ESP_LOGI(TAG, "idle after %u s: panel rail down, a key wakes it",
                 static_cast<unsigned>((now - last_touch_ms) / 1000));
        g_ui.idle(true);
        g_runtime.set_idle(true);
      }
    } else if (g_runtime.working() || g_key_while_idle) {
      // The trigger is the finger itself, not "a key in the last minute": a
      // timestamp window undid `epd idle on` for a minute after every boot, and
      // a console poll must never wake the glass.
      g_ui.idle(false);
      g_runtime.set_idle(false);
    }
    g_key_while_idle = false;
    // The screens' heartbeat: the recording counter, the playback position and
    // the panel's own redraw request. Nothing here runs on a timer of its own.
    g_ui.tick(now);
  }
}
