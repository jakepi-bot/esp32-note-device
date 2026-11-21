// Pairing a phone companion app over Bluetooth. The port owns the radio; the core
// owns the pairing window, its screen and the `phone` console command.
//
// The window alone does not pair anything: a phone that asks while it is open is
// named on the screen and waits (kPhoneAskMs) for a tap, which is the security
// acknowledgement — someone at the device decides, so a phone in radio range
// cannot bond itself to a device that is merely advertising.
#include "hg/app.hpp"

#include <cstdio>

namespace hg {

bool App::start_phone_pairing() {
  if (!on_phone_pair || prompt_showing() || ota_busy() || ota_ == Ota::Restarting) return false;
  if (mode_ == Mode::Listening) cancel_listening("phone pairing");
  else if (mode_ != Mode::Idle) cancel_turn();
  stop_playback();
  close_settings();
  close_wifi_setup();
  wake_display();
  talk_held_ = false;
  cancel_held_ = false;
  settings_chord_fired_ = false;
  wake_buttons_ = 0;
  if (!on_phone_pair()) {
    set_hint_flash(tr(i18n::Msg::BluetoothUnavailable));
    update_model();
    return false;
  }
  phone_pairing_ = true;
  phone_pair_until_ = now() + kPhonePairMs;
  phone_passkey_.clear();
  phone_ask_.clear();
  update_model();
  return true;
}

void App::close_phone_pairing() {
  if (!phone_pairing_) return;
  phone_pairing_ = false;
  phone_passkey_.clear();
  // A phone still waiting for the answer is refused by the port with the window.
  phone_ask_.clear();
  if (on_phone_pair_close) on_phone_pair_close();
  update_model();
}

void App::close_setup_screens() {
  close_wifi_setup();
  close_phone_pairing();
  close_camera();
}

void App::on_phone_ask(const uint8_t (&peer)[6]) {
  if (!phone_pairing_) return;
  char address[18];
  std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", peer[0], peer[1], peer[2],
                peer[3], peer[4], peer[5]);
  phone_ask_ = address;
  phone_ask_until_ = now() + kPhoneAskMs;
  wake_display();
  update_model();
}

void App::answer_phone_pairing(bool allow) {
  if (!phone_pairing_ || phone_ask_.empty()) return;
  phone_ask_.clear();
  if (on_phone_answer) on_phone_answer(allow);
  if (!allow) set_hint_flash(tr(i18n::Msg::PhonePairingFailed));
  update_model();
}

void App::on_phone_passkey(uint32_t passkey) {
  if (!phone_pairing_) return;
  char code[8];
  std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(passkey % 1000000));
  phone_ask_.clear();
  phone_passkey_ = code;
  wake_display();
  update_model();
}

void App::on_phone_paired(bool ok) {
  if (!phone_pairing_) return;
  close_phone_pairing();
  set_hint_flash(tr(ok ? i18n::Msg::PhonePaired : i18n::Msg::PhonePairingFailed));
  update_model();
}

void App::on_phone_status(int paired, bool connected) {
  phones_paired_ = paired < 0 ? 0 : paired;
  phone_connected_ = connected;
  if (settings_open()) update_model();
}

void App::phone_tick() {
  if (!phone_pairing_) return;
  if (!phone_ask_.empty() && static_cast<int32_t>(now() - phone_ask_until_) >= 0) {
    // Nobody answered: the phone is refused, and the window stays open for a retry.
    answer_phone_pairing(false);
    return;
  }
  if (static_cast<int32_t>(now() - phone_pair_until_) >= 0) {
    close_phone_pairing();
    set_hint_flash(tr(i18n::Msg::PhonePairingClosed));
    update_model();
  }
}

json::Value App::phone_value() const {
  json::Value v = json::Value::object();
  v.set("paired", phones_paired_).set("connected", phone_connected_).set("pairing", phone_pairing_);
  // What the screen is waiting on, for the console and the dashboard.
  if (!phone_ask_.empty()) v.set("confirm", phone_ask_);
  else if (!phone_passkey_.empty()) v.set("code", phone_passkey_);
  return v;
}

}  // namespace hg
