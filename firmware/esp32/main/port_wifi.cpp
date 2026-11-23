// Station changes run on the app task. IDF callbacks only post link events.
#include "port.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "lwip/inet.h"
#include "link_diag.hpp"

namespace hgp {
namespace {
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
esp_netif_t* station() { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
}

void Wifi::begin(NvsStorage& storage) {
  storage_ = &storage;
  fast_.load(storage.get(hg::WifiFastJoin::kStorageKey));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  // Credentials remain in our NVS store until a phone setup succeeds.
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  // The driver logs "Haven't to connect to a suitable AP now!" once per internal
  // reconnect, five times a second, and on the UART console that flood starved
  // every console answer (2026-11-22: `wifi-set` produced no reply at all). Its
  // own retry cadence is not information worth five lines a second; our retry
  // policy logs its attempts.
  esp_log_level_set("wifi", ESP_LOG_ERROR);
#if CONFIG_PM_ENABLE
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-wifi-join", &attempt_lock_) != ESP_OK) attempt_lock_ = nullptr;
#endif
  ESP_ERROR_CHECK(esp_wifi_start());
  started_ = true;
}

void Wifi::hold_while_attempting() {
  const bool want = retry_.attempting();
  if (!attempt_lock_ || want == attempt_held_) return;
  attempt_held_ = want;
  // Tell the touch task first, so it is back on its INT edge before the chip stays awake.
  if (want) {
    EspSystem::set_sleep_held(true);
    esp_pm_lock_acquire(attempt_lock_);
  } else {
    esp_pm_lock_release(attempt_lock_);
    EspSystem::set_sleep_held(false);
  }
}

void Wifi::retry_now() {
  retry_.retry_now(now_ms());
}

bool Wifi::configure(const std::string& ssid, const std::string& password, const hg::WifiFastAp* aim) {
  wifi_config_t cfg = {};
  std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(cfg.sta.password, password.data(), password.size());
  cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  cfg.sta.pmf_cfg.capable = true;
  // Asked for when joining; only max modem sleep uses it (wifi_power.hpp).
  cfg.sta.listen_interval = hg::WifiPowerSave::kListenInterval;
  if (aim) {
    // The access point that last worked, on its channel: no scan first (wifi_fast.hpp).
    cfg.sta.bssid_set = true;
    std::memcpy(cfg.sta.bssid, aim->bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = aim->channel;
  }
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
  ssid_ = ssid;
  password_ = password;
  aimed_ = aim != nullptr;
#if CONFIG_HG_WIFI_REUSE_IP
  // Opt-in: take the last lease as a static address and skip DHCP on a fast join.
  // Every other join, and the fallback after a failed fast one, uses DHCP.
  esp_netif_t* sta = station();
  const esp_err_t stopped = sta && aim && aim->has_lease() ? esp_netif_dhcpc_stop(sta) : ESP_FAIL;
  if (stopped == ESP_OK || stopped == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    esp_netif_ip_info_t info = {};
    info.ip.addr = aim->ip;
    info.netmask.addr = aim->netmask;
    info.gw.addr = aim->gateway;
    if (esp_netif_set_ip_info(sta, &info) == ESP_OK) {
      static_ip_ = true;
      if (aim->dns) {
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = aim->dns;
        esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
      }
    } else {
      esp_netif_dhcpc_start(sta);
    }
  } else if (sta && static_ip_) {
    static_ip_ = false;
    esp_netif_dhcpc_start(sta);
  }
#endif
  return true;
}

void Wifi::remember_ap() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
  hg::WifiFastAp seen;
  seen.ssid = ssid_;
  std::memcpy(seen.bssid, ap.bssid, sizeof(seen.bssid));
  seen.channel = ap.primary;
  esp_netif_t* sta = station();
  esp_netif_ip_info_t ip = {};
  if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
    seen.ip = ip.ip.addr;
    seen.netmask = ip.netmask.addr;
    seen.gateway = ip.gw.addr;
  }
  esp_netif_dns_info_t dns = {};
  if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
    seen.dns = dns.ip.u_addr.ip4.addr;
  if (auto record = fast_.joined(seen)) storage_->set(hg::WifiFastJoin::kStorageKey, *record);
}

void Wifi::forget_unsaved_ap() {
  // The network was forgotten: so is where its access point was.
  const auto& cached = fast_.cached();
  if (!cached || cached->ssid == CONFIG_HG_DEFAULT_WIFI_SSID || networks_.find(cached->ssid) < networks_.size()) return;
  if (fast_.forget()) storage_->erase(hg::WifiFastJoin::kStorageKey);
}

void Wifi::join(const std::string& ssid, const std::string& password) {
  configured_ = false;
  loss_applied_ = false;  // a new attempt: its loss, if any, is a fresh one
  if (scanning_) esp_wifi_scan_stop();
  scanning_ = false;
  retry_.connected();  // nothing pending until this join is under way
  hold_while_attempting();
  esp_wifi_disconnect();
  if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
    events::post(EventType::NetDown, "Wi-Fi not configured", 20);
    return;
  }
  if (!configure(ssid, password, fast_.aim(ssid))) return;
  configured_ = true;
  events::post(EventType::NetDown, "Joining Wi-Fi", 13);
  retry_.started(now_ms());
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now_ms());
  hold_while_attempting();
}

void Wifi::reconfigure() {
  joining_ = false;
  candidate_ = {};
  networks_ = hg::WifiNetworks::load(*storage_);
  if (networks_.dirty()) networks_.save(*storage_);
  forget_unsaved_ap();
  pick_.reset(networks_.size());
  // With nothing saved, the network built into the firmware (if any).
  const std::string ssid = networks_.empty() ? CONFIG_HG_DEFAULT_WIFI_SSID : networks_.all()[0].ssid;
  const std::string pass = networks_.empty() ? CONFIG_HG_DEFAULT_WIFI_PASSWORD : networks_.all()[0].password;
  auto_setup_ = ssid.empty() && !auto_setup_tried_;
  join(ssid, pass);
}

void Wifi::networks_changed() {
  networks_ = hg::WifiNetworks::load(*storage_);
  if (networks_.dirty()) networks_.save(*storage_);
  forget_unsaved_ap();
  // On the device's network, the device has moved it first (connected()).
  const bool on_first = link_up_ && !networks_.empty() && networks_.all()[0].ssid == ssid_ &&
                        networks_.all()[0].password == password_;
  if (on_first) pick_.reset(networks_.size());
  else reconfigure();
}

void Wifi::disconnected() {
  wait_disconnect_ = false;
  const bool already = loss_applied_;
  loss_applied_ = true;
  link_up_ = false;
  // The pass and the event both see the same loss. The backoff, the network
  // order and the PM lock must be touched once, so the second report stops
  // here (`loss_applied_` is cleared by the next join).
  if (already) return;
  if (configured_) {
    if (fast_.failed()) {
      // The fast join didn't work: join normally at once, without counting it against
      // the network or lengthening the battery backoff.
      retry_.lost(now_ms(), false);
      retry_.retry_now(now_ms());
    } else {
      pick_.failed();
      retry_.lost(now_ms(), on_battery && on_battery());
    }
  }
  // Stop the driver's own reconnect loop. Left alone it retries every 200 ms for
  // an access point that is not there, which floods the console and keeps the
  // radio busy between our attempts; `retry_` above is the cadence that counts.
  esp_wifi_disconnect();
  hold_while_attempting();
}

void Wifi::connected() {
  // The pass may have applied this association already (sta_linked()): the
  // second report of the same link has nothing to do.
  if (link_up_) return;
  retry_.connected();
  pick_.connected();
  hold_while_attempting();
  link_up_ = true;
  loss_applied_ = false;
  remember_ap();
  if (!joining_ && networks_.joined(ssid_)) {
    networks_.save(*storage_);
    pick_.reset(networks_.size());
  }
  if (!joining_ || wait_disconnect_) return;
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
      std::strncmp(reinterpret_cast<const char*>(ap.ssid), candidate_.ssid, sizeof(ap.ssid)) != 0) return;
  // Set up from the phone page: saved first, alongside the others.
  networks_ = hg::WifiNetworks::load(*storage_);
  networks_.put_first(candidate_.ssid, candidate_.password, true);
  networks_.save(*storage_);
  pick_.reset(networks_.size());
  // The settings the setup page posted. The app's console is what applies
  // them (the notes app answers only the ones it has): this port never parses
  // a setting of its own.
  if (on_console) {
    on_console(std::string("set server ") + candidate_.server);
    if (candidate_.tailnet_given) {
      // The key first: `tailnet on` starts the node with whatever key is saved.
      if (candidate_.ts_key[0]) on_console(std::string("set ts_key ") + candidate_.ts_key);
      on_console(std::string("set tailnet ") + (candidate_.tailnet_on ? "on" : "off"));
    }
  }
  candidate_ = {};
  joining_ = false;
  setup_status("connected");
  close_at_ = now_ms() + 10000;
  esp_netif_ip_info_t station{};
  auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta && esp_netif_get_ip_info(sta, &station) == ESP_OK &&
      (station.ip.addr & station.netmask.addr) == (inet_addr("192.168.4.1") & station.netmask.addr)) {
    // A phone that provisioned the device is done with the setup page: the
    // app's screen, when it has one, is what closes; otherwise the port does.
    if (close_setup) close_setup();
    else stop_setup();
  }
}

void Wifi::set_credentials(const std::string& ssid, const std::string& password) {
  if (ssid.empty() || ssid.size() > 32 || password.size() > 64) return;
  // Save first, so a reboot still has the network even if this join fails; then
  // put it at the head of the list and join it now.
  networks_ = hg::WifiNetworks::load(*storage_);
  networks_.add(ssid, password);
  networks_.move_first(ssid);
  networks_.save(*storage_);
  pick_.reset(networks_.size());
  join(ssid, password);
}

void Wifi::provision(const hg::WifiCredentials& credentials) {
  if (!http_ || joining_) return;
  candidate_ = credentials;
  wifi_ap_record_t ap = {};
  wait_disconnect_ = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  joining_ = true;
  trial_at_ = now_ms();
  setup_status("joining");
  join(candidate_.ssid, candidate_.password);
}

void Wifi::tick(uint32_t now) {
  if (auto_setup_) {
    auto_setup_ = false;
    auto_setup_tried_ = true;
    if (open_setup) open_setup();
    else start_setup();
  }
  if (joining_ && now - trial_at_ >= 30000) {
    reconfigure();
    setup_status("failed");
    std::lock_guard<std::mutex> lock(setup_mutex_);
    accepting_setup_ = true;
  }
  if (http_ && (static_cast<int32_t>(now - setup_until_) >= 0 ||
                (close_at_ && static_cast<int32_t>(now - close_at_) >= 0))) {
    if (close_setup) close_setup();
    else stop_setup();
  }
  if (configured_ && retry_.due(now)) attempt(now);
  // The link, as the driver has it. The notes app keeps no hg::App and so has
  // no NetUp handler to call connected() from: without this poll its window
  // would join the access point and still report no station, which leaves SNTP
  // unstarted and the uploader with nothing to send through. The transition
  // functions are idempotent, so the app's own event handler is not competing.
  if (started_) {
    if (sta_linked()) connected();
    else if (link_up_) disconnected();
  }
  hold_while_attempting();
  apply_power_save(now);
}

bool Wifi::sta_linked() const {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;  // not associated
  auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info = {};
  return sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0;
}

// The duty cycle's off half. The association goes first (the event arrives as
// a disconnect, which is why `configured_` is cleared before it), then the
// station itself: a station that is up but unassociated still holds the chip
// out of light sleep and costs the radio's idle current.
void Wifi::radio_off() {
  if (!started_) return;
  if (http_ || joining_) return;  // the setup page and a provisioning join are windows of their own
  configured_ = false;
  link_up_ = false;
  retry_.connected();  // no backoff carried into the next window
  esp_wifi_disconnect();
  if (esp_wifi_stop() != ESP_OK) {
    ESP_LOGW("hg.wifi", "the station did not stop; it stays up, unassociated");
    return;
  }
  started_ = false;
  hold_while_attempting();
  ESP_LOGI("hg.wifi", "radio off: the station is stopped, no association is held");
}

// The window's other half: start the station and join the first saved network.
// A join that fails is retried by the port's own clock while the window lasts.
void Wifi::radio_on() {
  if (!started_) {
    if (esp_wifi_start() != ESP_OK) {
      ESP_LOGW("hg.wifi", "the station did not start: the window has no network");
      return;
    }
    started_ = true;
  }
  ESP_LOGI("hg.wifi", "radio on: joining the saved network");
  reconfigure();
}

void Wifi::attempt(uint32_t now) {
  scanning_ = false;
  // Not while a phone setup tries its network.
  if (!joining_ && pick_.should_scan()) {
    wifi_scan_config_t scan = {};
    if (esp_wifi_scan_start(&scan, false) == ESP_OK) {
      scanning_ = true;  // scan_done() makes the attempt
      return;
    }
  }
  // Fast while the last access point hasn't failed since the last join; otherwise drop the aim.
  const hg::WifiFastAp* aim = joining_ ? nullptr : fast_.aim(ssid_);
  if ((aim || aimed_) && !configure(ssid_, password_, aim)) {
    retry_.start_failed(now);
    return;
  }
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now);
}

void Wifi::scan_done() {
  uint16_t found = 0;
  esp_wifi_scan_get_ap_num(&found);
  std::vector<wifi_ap_record_t> records(std::min<uint16_t>(found, 20));
  uint16_t count = static_cast<uint16_t>(records.size());
  if (count) esp_wifi_scan_get_ap_records(&count, records.data());  // frees the scan's list
  else esp_wifi_clear_ap_list();
  if (!scanning_ || !configured_ || joining_ || networks_.empty()) return;
  scanning_ = false;
  std::vector<std::string> saved;
  for (const auto& w : networks_.all()) saved.push_back(w.ssid);
  std::vector<hg::WifiPick::Seen> seen;
  for (uint16_t i = 0; i < count; ++i) {
    const char* name = reinterpret_cast<const char*>(records[i].ssid);
    seen.push_back({std::string_view(name, strnlen(name, sizeof(records[i].ssid))), records[i].rssi});
  }
  const hg::WifiNetwork& next = networks_.all()[pick_.choose(saved, seen)];
  ESP_LOGI("hg.wifi", "scan saw %u networks, trying %s", static_cast<unsigned>(found), next.ssid.c_str());
  if ((next.ssid != ssid_ || next.password != password_ || aimed_) && !configure(next.ssid, next.password)) {
    retry_.start_failed(now_ms());
    return;
  }
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now_ms());
}

void Wifi::apply_power_save(uint32_t now) {
  // Modem sleep only on battery and outside a turn, deeper while dozing quietly
  // (wifi_power.hpp) unless `wifi_deep_sleep` is off; a board without a battery
  // reading counts as plugged in.
  const auto want = power_save_.change(now, [this] { return on_battery && on_battery(); },
                                       in_turn && in_turn(), dozing && dozing() && deep_sleep_ok && deep_sleep_ok(),
                                       traffic_at ? traffic_at() : 0u);
  if (!want) return;
  static constexpr wifi_ps_type_t kMode[] = {WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM};
  if (esp_wifi_set_ps(kMode[static_cast<int>(*want)]) != ESP_OK) return;
  power_save_.applied(*want);
  ESP_LOGI("hg.wifi", "power save: %s", hg::wifi_sleep_name(*want));
}

void Wifi::on_event(void*, const char* base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    events::post(EventType::WifiStarted);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    events::post(EventType::WifiScanDone);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* info = static_cast<wifi_event_sta_disconnected_t*>(data);
    // "Wi-Fi lost (reason 200 beacon timeout, rssi -78)": the reason and the signal when it went.
    const std::string detail = hg::link::wifi_lost(info ? info->reason : 0, info ? info->rssi : 0);
    ESP_LOGW("hg.wifi", "%s", detail.c_str());
    events::post(EventType::WifiDisconnected, detail.data(), detail.size());
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* got = static_cast<ip_event_got_ip_t*>(data);
    char detail[32];
    std::snprintf(detail, sizeof(detail), IPSTR, IP2STR(&got->ip_info.ip));
    ESP_LOGI("hg.wifi", "connected, ip %s", detail);
    events::post(EventType::NetUp, detail, std::strlen(detail));
  }
}

}  // namespace hgp
