// Phone companion link over Bluetooth LE (NimBLE). See PhoneLink in port.hpp and
// docs/protocol.md#phone-app-over-bluetooth.
//
// Threads: NimBLE's host task runs every GAP and GATT callback, and every call
// here that changes advertising or the bond store is moved onto it with an
// event. A worker task hands command lines to the app task as console requests
// (like the serial console) and notifies the replies.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_random.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hg/radio_window.hpp"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);
#endif

namespace hgp {
namespace {
const char* TAG = "hg.phone";
}  // namespace

#if !CONFIG_BT_NIMBLE_ENABLED

bool PhoneLink::begin(const std::string&) {
  ESP_LOGE(TAG, "this board enables Bluetooth, but the build has no NimBLE: add the Bluetooth lines to its "
                "sdkconfig.defaults (docs/hardware-validation.md#bluetooth)");
  return false;
}
bool PhoneLink::open_pairing() { return false; }
void PhoneLink::tick(uint32_t) {}
void PhoneLink::note_saved() {}
void PhoneLink::wake() {}
PhoneOpen PhoneLink::open_phone_window() { return PhoneOpen::BleOff; }
PhoneWindow PhoneLink::phone_window() const { return PhoneWindow{}; }
void PhoneLink::close_phone_window() {}
void PhoneLink::set_pending(uint32_t) {}
bool PhoneLink::radio_up() const { return false; }
const char* PhoneLink::radio_reason() const { return "none"; }
void PhoneLink::answer_pairing(bool) {}
void PhoneLink::close_pairing() {}
void PhoneLink::forget() {}
bool PhoneLink::bonded_peers(BondInfo*, uint8_t, uint8_t* count) {
  if (count != nullptr) *count = 0;
  return false;
}
bool PhoneLink::forget_peer(const BondInfo&) { return false; }
void PhoneLink::set_name(const std::string&) {}

// The note sync lives on the same stack; without NimBLE there is nothing to
// register and the notes mode never asks.
bool NoteLink::available() const { return false; }
bool NoteLink::connected() const { return false; }
uint16_t NoteLink::mtu() const { return 23; }
bool NoteLink::poll_command(std::string&) { return false; }
bool NoteLink::send_frame(const uint8_t*, size_t) { return false; }
void NoteLink::note_saved() {}
void NoteLink::pending(uint32_t) {}

#else

namespace {

constexpr size_t kMaxLine = 256;        // the serial console's line limit too
constexpr uint32_t kReplyWaitMs = 3000;  // as long as the serial console waits
constexpr uint32_t kForgetDropMs = 1000;  // lets the reply to `phone forget` leave first
constexpr uint16_t kNone = BLE_HS_CONN_HANDLE_NONE;

// dc530001-7a2a-400f-b8f6-e87185576192 and its characteristics ...0002 (command)
// and ...0003 (reply), little-endian as NimBLE stores them. The note sync's own
// service is ...00a0 with ...00a1 (control) and ...00a2 (data) beside it.
constexpr uint8_t kUuidBase[16] = {0x92, 0x61, 0x57, 0x85, 0x71, 0xe8, 0xf6, 0xb8,
                                   0x0f, 0x40, 0x2a, 0x7a, 0x00, 0x00, 0x53, 0xdc};
ble_uuid128_t g_service_uuid, g_command_uuid, g_reply_uuid;
ble_uuid128_t g_note_service_uuid, g_note_control_uuid, g_note_data_uuid;
ble_gatt_chr_def g_chrs[3];
ble_gatt_chr_def g_note_chrs[3];
ble_gatt_svc_def g_svcs[3];
uint16_t g_reply_handle = 0;
uint16_t g_note_control_handle = 0;
uint16_t g_note_data_handle = 0;

struct Line {
  uint16_t conn;
  uint16_t len;
  char text[kMaxLine + 1];
};
QueueHandle_t g_lines = nullptr;

// One command line from the note-sync control characteristic, waiting for the
// app task. `resume <id> <offset>` is the longest thing the phone can send.
constexpr size_t kNoteLine = 96;
struct NoteLine {
  uint16_t len;
  char text[kNoteLine + 1];
};
QueueHandle_t g_note_lines = nullptr;
std::atomic<bool> g_note_subscribed{false};
std::atomic<uint16_t> g_note_mtu{23};
std::atomic<bool> g_note_ready{false};

bool g_started = false;  // app task: begin() succeeded
std::atomic<bool> g_synced{false}, g_pairing{false};
// The radio's clock (hg/radio_window.hpp): off at rest, on inside a window.
// `g_up` is true only while the controller and host are running.
hg::RadioWindow g_window;
std::atomic<bool> g_up{false};
bool g_prepared = false;  // app task: the queues and the worker exist

// The bond store's size and count. `slots` is what the pairing table holds
// (CONFIG_BT_NIMBLE_MAX_BONDS, three on this build); `g_bonds` is the count,
// cached because ble_store_util_count() walks NVS and the Phone screen polls it.
constexpr uint8_t kBondSlots = CONFIG_BT_NIMBLE_MAX_BONDS;
std::atomic<uint8_t> g_bonds{0};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Shared by PhoneLink and NoteLink: both are this one radio (port.hpp).
void radio_note_saved() {
  if (!g_prepared) return;
  g_window.note_saved(now_ms());
}
void radio_set_pending(uint32_t count) {
  if (!g_prepared) return;
  g_window.set_pending(count, now_ms());
}
std::atomic<uint16_t> g_conn{kNone};
uint16_t g_pairing_conn = kNone;  // host task only: the connection shown a passkey
uint8_t g_own_addr_type = 0;
std::mutex g_name_mutex;
std::string g_name;

ble_npl_event g_update_ev, g_forget_ev, g_answer_ev;
ble_npl_callout g_drop;

// A phone waiting for the screen's answer (the port's pairing acknowledgement,
// docs/protocol.md#phone-app-over-bluetooth). Written on the host task, read by
// the app task, answered back on the host task through g_answer_ev.
struct Ask {
  uint16_t conn = kNone;
  uint32_t passkey = 0;
  uint8_t peer[6] = {};
  bool open = false;
};
std::mutex g_ask_mutex;
Ask g_ask;
std::atomic<bool> g_allow{false};

// The bond table, for the Unpair screen. The store's own API (ble_store_util_*)
// belongs to the host task: it walks the host's cached copy of the bond store.
// The screens ask from the app task, so the work goes onto the host's event queue
// and the app waits a second for the answer. With the radio at rest there is no
// host to hand it to and none to read either - nimble_port_deinit() took the store
// and its lock with it - so the call reports "no answer" and the screen keeps the
// table it already had. One job at a time: the app task is the only caller.
constexpr uint8_t kBondCap = 6;        // notesui::kMaxPeerRows
constexpr uint32_t kBondWaitMs = 1000;  // NVS plus a RAM cache, and the host's lock

struct BondJob {
  bool erase = false;
  ble_addr_t target{};
  bool ok = false;  // the erase was accepted (false when there was none)
  uint8_t count = 0;
  BondInfo peers[kBondCap];
  std::atomic<bool> done{false};
};
BondJob g_bond_job;
ble_npl_event g_bond_ev;

void make_uuid(ble_uuid128_t& uuid, uint8_t n) {
  uuid.u.type = BLE_UUID_TYPE_128;
  std::memcpy(uuid.value, kUuidBase, sizeof(kUuidBase));
  uuid.value[12] = n;
}

int bonds() {
  int count = 0;
  ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
  return count;
}

void post(PhoneSample::Kind kind, uint32_t passkey = 0, const uint8_t* peer = nullptr) {
  PhoneSample s{kind, passkey, 0, false, {}};
  if (kind == PhoneSample::Kind::Status) {
    g_bonds = static_cast<uint8_t>(bonds());
    s.paired = g_bonds.load();
    s.connected = g_conn.load() != kNone;
  }
  if (peer != nullptr) std::memcpy(s.peer, peer, sizeof(s.peer));
  events::post(EventType::Phone, &s, sizeof(s));
}

// A connection the console may be used from: encrypted with a paired phone's
// passkey-authenticated keys.
bool trusted(uint16_t conn) {
  ble_gap_conn_desc d;
  return ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated &&
         d.sec_state.bonded;
}

int on_gap(ble_gap_event* event, void*);

// Host task. Advertises fast while pairing is open, slowly (about once a second)
// for a paired phone to reconnect, and not at all otherwise. One phone at a time.
void advertise() {
  if (!g_synced || g_conn.load() != kNone) return;
  ble_gap_adv_stop();
  const bool pairing = g_pairing.load();
  if (!pairing && bonds() == 0) return;

  std::string name;
  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    name = g_name;
  }
  ble_svc_gap_device_name_set(name.c_str());
  ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  // The note service, not the hub service: a Web Bluetooth caller filters on the
  // service it is going to use, and Chrome and iOS drop any device whose
  // advertising packet does not carry it. Advertising ...0001 here (the hub's
  // UUID) left the phone's chooser empty, which is how a real iPhone saw "no
  // device". Both 128-bit UUIDs do not fit in one 31-byte packet with the flags,
  // and the phone only ever talks to ...00a0, so this one is the advertisement.
  fields.uuids128 = &g_note_service_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  ble_hs_adv_fields scan = {};
  if (name.size() > 29) name.resize(29);  // what fits in a scan response
  scan.name = reinterpret_cast<const uint8_t*>(name.data());
  scan.name_len = static_cast<uint8_t>(name.size());
  scan.name_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&scan);
  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = pairing ? BLE_GAP_ADV_FAST_INTERVAL1_MIN : BLE_GAP_ADV_ITVL_MS(1000);
  params.itvl_max = pairing ? BLE_GAP_ADV_FAST_INTERVAL1_MAX : BLE_GAP_ADV_ITVL_MS(1100);
  if (rc == 0) rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, on_gap, nullptr);
  if (rc != 0) ESP_LOGE(TAG, "can't advertise (%d)", rc);
}

void on_update(ble_npl_event*) { advertise(); }

void on_drop(ble_npl_event*) {
  const uint16_t conn = g_conn.load();
  if (conn != kNone) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

// Host task: the screen's answer to a pairing ask. Allowed, the passkey goes in
// and the phone shows the same six digits; refused (or timed out, which the app
// task sends the same way), the link is dropped and the phone learns nothing.
void on_answer(ble_npl_event*) {
  Ask ask;
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (!g_ask.open) return;
    ask = g_ask;
    g_ask = Ask{};
  }
  const bool allow = g_allow.load();
  if (!allow) {
    ESP_LOGW(TAG, "pairing refused on the screen");
    ble_gap_terminate(ask.conn, BLE_ERR_AUTH_FAIL);
    return;
  }
  ble_sm_io io = {};
  io.action = BLE_SM_IOACT_DISP;
  io.passkey = ask.passkey;
  const int rc = ble_sm_inject_io(ask.conn, &io);
  if (rc != 0) {
    ESP_LOGW(TAG, "pairing response not accepted (%d)", rc);
    ble_gap_terminate(ask.conn, BLE_ERR_AUTH_FAIL);
    return;
  }
  ESP_LOGI(TAG, "pairing allowed on the screen");
  post(PhoneSample::Kind::Passkey, ask.passkey);
}

void on_forget(ble_npl_event*) {
  ble_store_clear();
  ESP_LOGI(TAG, "paired phones removed");
  post(PhoneSample::Kind::Status);
  if (g_conn.load() != kNone) ble_npl_callout_reset(&g_drop, ble_npl_time_ms_to_ticks32(kForgetDropMs));
  else advertise();
}

// Host task (or the app task with the radio at rest). One walk of the store for
// both questions: the erase first when the job carries one, then the table as it
// is now - which is the answer the screen draws.
void schedule(ble_npl_event* event);

void run_bond_job() {
  if (g_bond_job.erase) {
    const int rc = ble_store_util_delete_peer(&g_bond_job.target);
    g_bond_job.ok = rc == 0;
    if (rc != 0) {
      ESP_LOGW(TAG, "the bond store refused the delete (%d)", rc);
    } else {
      // The keys are gone, so the link must go with them: the phone itself keeps
      // its side of the bond until it notices, and a link kept over keys this
      // device no longer has is a connection that can never be encrypted again.
      const uint16_t conn = g_conn.load();
      if (conn != kNone) {
        ble_gap_conn_desc d;
        if (ble_gap_conn_find(conn, &d) == 0 && ble_addr_cmp(&d.peer_id_addr, &g_bond_job.target) == 0) {
          ESP_LOGI(TAG, "the unpaired phone was connected: dropping the link");
          ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
      }
    }
  }
  ble_addr_t found[kBondCap];
  int n = 0;
  if (ble_store_util_bonded_peers(found, &n, kBondCap) == 0) {
    if (n < 0) n = 0;
    if (n > kBondCap) n = kBondCap;
    g_bond_job.count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
      g_bond_job.peers[i].type = found[i].type;
      // NimBLE keeps an address little-endian; the screen prints it the way the
      // phone's own settings do.
      for (int b = 0; b < 6; ++b) g_bond_job.peers[i].addr[b] = found[i].val[5 - b];
    }
  } else {
    g_bond_job.count = 0;
  }
  g_bonds = g_bond_job.count;  // the Phone screen's count line reads this
  g_bond_job.done = true;
  post(PhoneSample::Kind::Status);
  advertise();
}

void on_bond_job(ble_npl_event*) { run_bond_job(); }

// App task: the job, run on the host when there is one (the store is its) and
// inline when the radio is at rest.
bool ask_bond_store(bool erase, const ble_addr_t* target) {
  g_bond_job.erase = erase;
  g_bond_job.ok = false;
  g_bond_job.count = 0;
  if (erase && target != nullptr) g_bond_job.target = *target;
  g_bond_job.done = false;
  if (g_started) {
    schedule(&g_bond_ev);
    for (uint32_t waited = 0; waited < kBondWaitMs && !g_bond_job.done.load(); ++waited) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!g_bond_job.done.load()) {
      ESP_LOGW(TAG, "the bond store did not answer in %u ms", static_cast<unsigned>(kBondWaitMs));
      return false;
    }
  } else {
    run_bond_job();  // no host task, and nothing else reads the store
  }
  return true;
}

void schedule(ble_npl_event* event) { ble_npl_eventq_put(nimble_port_get_dflt_eventq(), event); }

int on_gap(ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) {
        advertise();
        return 0;
      }
      g_conn = event->connect.conn_handle;
      g_window.connected(true, now_ms());  // a link keeps the radio up
      g_note_subscribed = false;
      g_note_mtu = ble_att_mtu(event->connect.conn_handle);
      ESP_LOGI(TAG, "phone connected");
      post(PhoneSample::Kind::Status);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      g_conn = kNone;
      g_window.connected(false, now_ms());  // a short grace, then the radio may rest
      g_note_subscribed = false;
      g_note_mtu = 23;
      {
        std::lock_guard<std::mutex> lock(g_ask_mutex);
        if (g_ask.open && g_ask.conn == event->disconnect.conn.conn_handle) g_ask = Ask{};
      }
      if (g_pairing_conn == event->disconnect.conn.conn_handle) {
        g_pairing_conn = kNone;
        post(PhoneSample::Kind::Failed);
      }
      ESP_LOGI(TAG, "phone disconnected (%d)", event->disconnect.reason);
      post(PhoneSample::Kind::Status);
      advertise();
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      return 0;

    case BLE_GAP_EVENT_MTU:
      // The phone asked for a bigger ATT MTU: the note sync packs its
      // notifications to it, and a command has to fit one write.
      g_note_mtu = event->mtu.value;
      ESP_LOGI(TAG, "MTU %u", static_cast<unsigned>(event->mtu.value));
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == g_note_data_handle) {
        g_note_subscribed = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "note sync %s", event->subscribe.cur_notify ? "subscribed" : "unsubscribed");
      }
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      const uint16_t conn = event->passkey.conn_handle;
      if (event->passkey.params.action != BLE_SM_IOACT_DISP || !g_pairing) {
        ESP_LOGW(TAG, "pairing refused: %s", g_pairing ? "the phone asked for another method" : "pairing is closed");
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return 0;
      }
      // The phone does not get the code yet: the screen names it first, and the
      // passkey only goes in when the person holding the device allows it. A
      // second phone asking while one waits is turned away.
      ble_gap_conn_desc d;
      if (ble_gap_conn_find(conn, &d) != 0) {
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return 0;
      }
      uint8_t peer[6];
      const uint8_t* addr = d.peer_id_addr.val;
      for (int i = 0; i < 6; ++i) peer[i] = addr[5 - i];  // NimBLE keeps the address little-endian
      bool busy = false;
      {
        std::lock_guard<std::mutex> lock(g_ask_mutex);
        busy = g_ask.open;
        if (!busy) {
          g_ask.conn = conn;
          g_ask.passkey = esp_random() % 1000000;
          std::memcpy(g_ask.peer, peer, sizeof(peer));
          g_ask.open = true;
        }
      }
      if (busy) {
        ESP_LOGW(TAG, "pairing refused: another phone is waiting for the screen");
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return 0;
      }
      g_pairing_conn = conn;
      ESP_LOGI(TAG, "a phone asks to pair; waiting for the screen");
      post(PhoneSample::Kind::Ask, 0, peer);
      return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
      const uint16_t conn = event->enc_change.conn_handle;
      const bool pairing_conn = conn == g_pairing_conn;
      if (pairing_conn) g_pairing_conn = kNone;
      ble_gap_conn_desc d;
      if (ble_gap_conn_find(conn, &d) != 0) return 0;
      // Pairing that finishes after the window closed (CANCEL while the phone
      // still asked for the code) isn't kept either.
      const bool closed_too_soon = pairing_conn && !g_pairing;
      if (event->enc_change.status == 0 && d.sec_state.authenticated && d.sec_state.bonded && !closed_too_soon) {
        if (pairing_conn) {
          // The same address the ask carried, so the screen can tell this bond
          // from a second phone's question (t_da500270).
          uint8_t peer[6];
          for (int i = 0; i < 6; ++i) peer[i] = d.peer_id_addr.val[5 - i];
          ESP_LOGI(TAG, "phone paired");
          post(PhoneSample::Kind::Paired, 0, peer);
        }
        post(PhoneSample::Kind::Status);
        return 0;
      }
      // Without a passkey (Just Works) the keys prove nothing about who paired:
      // don't keep them.
      ESP_LOGW(TAG, "%s (%d); disconnecting",
               closed_too_soon ? "pairing finished after it closed" : "encryption without a paired phone's keys",
               event->enc_change.status);
      if (event->enc_change.status == 0) ble_store_util_delete_peer(&d.peer_id_addr);
      if (pairing_conn) post(PhoneSample::Kind::Failed);
      ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // A phone that forgot this device pairs again, but only while pairing is open.
      if (!g_pairing) return BLE_GAP_REPEAT_PAIRING_IGNORE;
      ble_gap_conn_desc d;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
  }
  return 0;
}

int on_access(uint16_t conn, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
  if (!trusted(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  Line line;
  line.conn = conn;
  uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  if (len == 0 || len > kMaxLine) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  if (ble_hs_mbuf_to_flat(ctxt->om, line.text, kMaxLine, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
  line.len = len;
  line.text[len] = 0;
  // One command at a time, plus one waiting: the phone waits for each reply.
  if (xQueueSend(g_lines, &line, 0) != pdTRUE) return BLE_ATT_ERR_INSUFFICIENT_RES;
  return 0;
}

int on_note_access(uint16_t conn, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
  // The control characteristic: one command line per write, only from a phone
  // paired with the passkey (`trusted`), and never longer than a line.
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
  if (!trusted(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  NoteLine line;
  uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  if (len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  if (len > kNoteLine) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  if (ble_hs_mbuf_to_flat(ctxt->om, line.text, kNoteLine, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
  line.len = len;
  line.text[len] = 0;
  // Four commands queue up while the app task is busy with a screen refresh;
  // more than that means the phone is not waiting for answers.
  if (xQueueSend(g_note_lines, &line, 0) != pdTRUE) return BLE_ATT_ERR_INSUFFICIENT_RES;
  return 0;
}

// Notifies `reply` and a closing NUL in pieces that fit the link's MTU, waiting
// for buffers when NimBLE runs out.
void send_reply(uint16_t conn, std::string reply) {
  reply.push_back('\0');
  const size_t chunk = ble_att_mtu(conn) > 3 ? ble_att_mtu(conn) - 3 : 20;
  for (size_t at = 0; at < reply.size();) {
    const size_t n = std::min(chunk, reply.size() - at);
    int rc = BLE_HS_ENOMEM;
    for (int tries = 0; tries < 200 && rc == BLE_HS_ENOMEM; ++tries) {
      os_mbuf* om = ble_hs_mbuf_from_flat(reply.data() + at, static_cast<uint16_t>(n));
      rc = om ? ble_gatts_notify_custom(conn, g_reply_handle, om) : BLE_HS_ENOMEM;  // frees om either way
      if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rc != 0) {
      ESP_LOGW(TAG, "reply not sent (%d)", rc);
      return;
    }
    at += n;
  }
}

void worker(void*) {
  Line line;
  for (;;) {
    if (xQueueReceive(g_lines, &line, portMAX_DELAY) != pdTRUE) continue;
    // The same path as the serial console (port_console.cpp).
    ConsoleRequest* req = ConsoleRequest::create();
    std::string reply;
    if (!events::post(EventType::Console, line.text, line.len, 0, req)) {
      req->release();  // nobody will answer: drop the app task's share too
      reply = "@error device busy";
    } else if (!req->wait(kReplyWaitMs)) {
      reply = "@error device busy";
    } else {
      reply = req->reply();
    }
    req->release();
    if (line.conn == g_conn.load()) send_reply(line.conn, std::move(reply));
  }
}

void on_sync() {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &g_own_addr_type);
  g_synced = true;
  post(PhoneSample::Kind::Status);
  advertise();
}

void on_reset(int reason) {
  g_synced = false;
  ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason);
}

void host_task(void*) {
  nimble_port_run();  // returns only after nimble_port_stop()
  nimble_port_freertos_deinit();
}

}  // namespace

// App task. Brings the controller and the NimBLE host up. Every call is after a
// previous stop_radio() or at boot; the bond store (NVS) survives, so a paired
// phone is still paired when the radio comes back.
bool start_radio() {
  if (g_up.load()) return true;
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth didn't start: %s", esp_err_to_name(err));
    return false;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;  // a full store forgets the oldest phone
  // This device shows a passkey and the phone types it: LE Secure Connections
  // with protection against a man in the middle, keys kept for reconnecting.
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(g_svcs);
  if (rc == 0) rc = ble_gatts_add_svcs(g_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "can't register the phone service (%d)", rc);
    nimble_port_deinit();
    return false;
  }
  g_note_ready = true;
  ble_store_config_init();
  ble_npl_event_init(&g_update_ev, on_update, nullptr);
  ble_npl_event_init(&g_forget_ev, on_forget, nullptr);
  ble_npl_event_init(&g_answer_ev, on_answer, nullptr);
  ble_npl_event_init(&g_bond_ev, on_bond_job, nullptr);
  ble_npl_callout_init(&g_drop, nimble_port_get_dflt_eventq(), on_drop, nullptr);
  nimble_port_freertos_init(host_task);
  g_up = true;
  g_started = true;
  ESP_LOGI(TAG, "Bluetooth ready (%s)", hg::RadioWindow::name(g_window.why(now_ms())));
  return true;
}

// App task. The reverse: the host stops, then the controller is disabled and
// deinitialised, which is what lets the chip light-sleep again. A phone that is
// connected keeps the radio up (the window policy says so), so this refuses
// while a link exists rather than dropping someone mid-transfer.
bool stop_radio() {
  if (!g_up.load()) return true;
  if (g_conn.load() != kNone) return false;
  g_synced = false;
  g_pairing = false;
  g_note_subscribed = false;
  g_note_ready = false;
  g_started = false;
  const esp_err_t stopped = nimble_port_stop();
  if (stopped != ESP_OK) {
    // The host refused to stop: the controller stays as it is and the next tick
    // tries again rather than leaving a half-stopped stack behind.
    ESP_LOGW(TAG, "Bluetooth host did not stop (%s)", esp_err_to_name(stopped));
    g_up = false;
    return false;
  }
  const esp_err_t err = nimble_port_deinit();
  g_up = false;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Bluetooth controller did not deinit (%s)", esp_err_to_name(err));
    return false;
  }
  // The npl handles are pointers into the host's own memory, which deinit has
  // just freed. Cleared here, so the next start_radio() callocs fresh ones; left
  // set, `npl_freertos_event_init` memsets through a stale pointer, and the
  // second window after the radio rested (the boot window closing, then a note
  // saved or `phone wake`) panics with StoreProhibited at 0xa5a5a5a5.
  g_update_ev.event = nullptr;
  g_forget_ev.event = nullptr;
  g_answer_ev.event = nullptr;
  g_bond_ev.event = nullptr;
  g_drop.co = nullptr;
  ESP_LOGI(TAG, "Bluetooth radio off: %s", "the chip can light-sleep again");
  return true;
}

bool PhoneLink::begin(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    g_name = name;
  }
  g_lines = xQueueCreate(2, sizeof(Line));
  g_note_lines = xQueueCreate(4, sizeof(NoteLine));
  if (!g_lines || !g_note_lines || xTaskCreate(worker, "hg_phone", 4096, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "out of memory for the phone link");
    return false;
  }

  make_uuid(g_service_uuid, 0x01);
  make_uuid(g_command_uuid, 0x02);
  make_uuid(g_reply_uuid, 0x03);
  g_chrs[0] = {};
  g_chrs[0].uuid = &g_command_uuid.u;
  g_chrs[0].access_cb = on_access;
  g_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
  g_chrs[1] = {};
  g_chrs[1].uuid = &g_reply_uuid.u;
  g_chrs[1].access_cb = on_access;
  g_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  g_chrs[1].val_handle = &g_reply_handle;
  g_chrs[2] = {};
  g_svcs[0] = {};
  g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_svcs[0].uuid = &g_service_uuid.u;
  g_svcs[0].characteristics = g_chrs;

  // The note sync (docs/ble-note-sync.md): the phone writes commands to the
  // control characteristic and subscribes to the data one for the packets.
  // Both need the same paired phone: a note is private, and an unencrypted
  // link would let anyone in radio range read it.
  make_uuid(g_note_service_uuid, 0xa0);
  make_uuid(g_note_control_uuid, 0xa1);
  make_uuid(g_note_data_uuid, 0xa2);
  g_note_chrs[0] = {};
  g_note_chrs[0].uuid = &g_note_control_uuid.u;
  g_note_chrs[0].access_cb = on_note_access;
  g_note_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
  g_note_chrs[0].val_handle = &g_note_control_handle;
  g_note_chrs[1] = {};
  g_note_chrs[1].uuid = &g_note_data_uuid.u;
  // NimBLE refuses a characteristic with no access callback (ble_gatts_chr_is_sane),
  // and ble_gatts_count_cfg() then fails with BLE_HS_EINVAL before any service is
  // registered. The data characteristic is notify-only, so on_note_access rejects
  // every operation on it; the callback is here for the table validator.
  g_note_chrs[1].access_cb = on_note_access;
  g_note_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  g_note_chrs[1].val_handle = &g_note_data_handle;
  g_note_chrs[2] = {};
  g_svcs[1] = {};
  g_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_svcs[1].uuid = &g_note_service_uuid.u;
  g_svcs[1].characteristics = g_note_chrs;
  g_svcs[2] = {};

  g_prepared = true;
  // Boot opens the first window so a phone that is looking finds a new device;
  // every later window has its own trigger (`phone pair`, a recorded note, a
  // waiting note, an explicit wake).
  g_window.boot(now_ms());
  return start_radio();
}

bool PhoneLink::open_pairing() {
  if (!g_prepared) {
    ESP_LOGW(TAG, "pairing refused: this build has no Bluetooth");
    return false;
  }
  // The exchange itself needs the host: open the radio window first, and the
  // tick that follows brings the controller up (or leaves it up). The deadline
  // is logged because "it returned true and nothing happened" is exactly the
  // report this bench had to rule out.
  g_window.pair(now_ms());
  g_pairing = true;
  const bool was_up = g_up.load();
  if (!was_up && !start_radio()) {
    ESP_LOGW(TAG, "pairing open on paper, but the controller did not start");
    return false;
  }
  if (g_started) schedule(&g_update_ev);
  ESP_LOGI(TAG, "pairing open for %u ms: radio %s, %u paired of %u", static_cast<unsigned>(g_window.timing().pairing_ms),
           g_up.load() ? "up" : "starting", g_bonds.load(), kBondSlots);
  return true;
}

// Menu -> Phone. The window, the pairing flag and the controller are brought up
// together here, so the screen's five minutes and the console's `phone pair`
// differ only in how long they last; every refusal is named, because a row that
// flashes and returns Home is the defect this exists to end (t_9972e2a1).
PhoneOpen PhoneLink::open_phone_window() {
  if (!g_prepared) {
    ESP_LOGW(TAG, "phone window refused: this build has no Bluetooth");
    return PhoneOpen::BleOff;
  }
  const uint8_t paired = static_cast<uint8_t>(bonds());
  g_bonds = paired;
  if (paired >= kBondSlots) {
    // Advertising with no slot to keep the phone in: the store's own policy
    // (`ble_store_util_status_rr`) would evict the oldest partner mid-exchange,
    // which is worse than saying no.
    ESP_LOGW(TAG, "phone window refused: the pairing table is full (%u of %u)", paired, kBondSlots);
    return PhoneOpen::TableFull;
  }
  g_window.phone(now_ms());
  g_pairing = true;
  if (!g_up.load() && !start_radio()) {
    ESP_LOGE(TAG, "phone window refused: the controller did not start");
    return PhoneOpen::RadioBusy;
  }
  if (g_started) schedule(&g_update_ev);  // advertise now, fast: a phone is looking
  const uint32_t left = g_window.phone_left_ms(now_ms());
  ESP_LOGI(TAG, "phone window open: %u of %u paired, %u ms on the clock, radio %s%s", paired, kBondSlots,
           static_cast<unsigned>(left), g_up.load() ? "up" : "starting",
           g_conn.load() != kNone ? ", a phone is connected" : "");
  return PhoneOpen::Opened;
}

PhoneWindow PhoneLink::phone_window() const {
  PhoneWindow w;
  w.slots = kBondSlots;
  w.paired = g_bonds.load();
  w.ready = g_up.load();
  w.connected = g_conn.load() != kNone;
  w.left_ms = g_window.phone_left_ms(now_ms());
  return w;
}

void PhoneLink::close_phone_window() {
  if (!g_prepared) return;
  const uint32_t left = g_window.phone_left_ms(now_ms());
  if (left == 0) return;  // already over: closing twice is not an event
  g_window.phone_closed(now_ms());
  ESP_LOGI(TAG, "phone window closed by the screen with %u s to run", static_cast<unsigned>(left / 1000));
}

void PhoneLink::answer_pairing(bool allow) {
  if (!g_started) return;
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (!g_ask.open) return;
  }
  g_allow = allow;
  schedule(&g_answer_ev);
}

void PhoneLink::close_pairing() {
  if (!g_started || !g_pairing.exchange(false)) return;
  {
    // A phone still waiting for the screen is refused with the window.
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (g_ask.open) {
      g_allow = false;
      schedule(&g_answer_ev);
    }
  }
  ESP_LOGI(TAG, "pairing closed");
  schedule(&g_update_ev);
}

void PhoneLink::forget() {
  if (g_started) schedule(&g_forget_ev);
}

bool PhoneLink::bonded_peers(BondInfo* out, uint8_t capacity, uint8_t* count) {
  if (count != nullptr) *count = 0;
  // The store is the host's: nimble_port_deinit() frees it with the rest of the
  // stack, so a read while the radio rests is a read of freed memory (the app
  // task panicked on ble_hs_lock the moment the Unpair row was selected with the
  // radio off). A caller that needs the table brings the radio up first.
  if (out == nullptr || capacity == 0 || !g_started) return false;
  if (!ask_bond_store(false, nullptr)) return false;
  const uint8_t n = capacity < g_bond_job.count ? capacity : g_bond_job.count;
  for (uint8_t i = 0; i < n; ++i) out[i] = g_bond_job.peers[i];
  if (count != nullptr) *count = n;
  return true;
}

bool PhoneLink::forget_peer(const BondInfo& peer) {
  if (!g_started) return false;  // no host, no store (see bonded_peers)
  ble_addr_t target{};
  target.type = peer.type;
  for (int b = 0; b < 6; ++b) target.val[b] = peer.addr[5 - b];  // back to NimBLE's order
  if (!ask_bond_store(true, &target)) return false;
  if (!g_bond_job.ok) return false;
  // The answer the screen draws is the table the store holds now: the phone has
  // to be gone from it, or the delete did not take.
  for (uint8_t i = 0; i < g_bond_job.count; ++i) {
    if (std::memcmp(g_bond_job.peers[i].addr, peer.addr, sizeof peer.addr) == 0) return false;
  }
  return true;
}

void PhoneLink::set_name(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    g_name = name;
  }
  if (g_started) schedule(&g_update_ev);
}

// -- the radio's clock (hg/radio_window.hpp) -----------------------------------

void PhoneLink::tick(uint32_t now_ms_arg) {
  if (!g_prepared) return;
  // A link that is up keeps the window warm; the policy reads that from the
  // connect/disconnect events, so all this has to do is bring the controller
  // up inside a window and stop it once the window (and its grace) is over.
  const bool phone_was_open = g_window.phone_left_ms(now_ms_arg) != 0;
  const bool want = g_window.tick(now_ms_arg);
  // The pairing exchange is allowed exactly as long as one of its windows is
  // open. Derived here rather than set and forgotten: the notes app opened a
  // pairing window at boot and never closed it, so the device advertised as
  // pairable long after every window had expired.
  const bool pairing_want = g_window.pairing_open(now_ms_arg);
  if (pairing_want != g_pairing.load()) {
    if (pairing_want) {
      g_pairing = true;
    } else {
      close_pairing();  // also refuses a phone still waiting for the screen
    }
  }
  // The Phone screen's window ran out on its own. This is the one place that
  // sees the deadline pass, so it is where the reason it closed is written.
  const uint32_t phone_left = g_window.phone_left_ms(now_ms_arg);
  if (phone_was_open && phone_left == 0) {
    ESP_LOGI(TAG, "phone window closed on its own: %u of %u paired, %s, radio %s", g_bonds.load(), kBondSlots,
             g_conn.load() != kNone ? "a phone is still connected" : "no phone connected",
             want ? hg::RadioWindow::name(g_window.why(now_ms_arg)) : "going off");
  }
  if (want && !g_up.load()) {
    start_radio();
  } else if (!want && g_up.load()) {
    stop_radio();
  }
}

void PhoneLink::note_saved() { radio_note_saved(); }

void PhoneLink::wake() {
  if (!g_prepared) {
    ESP_LOGW(TAG, "wake refused: this build has no Bluetooth");
    return;
  }
  g_window.trigger(now_ms());
  if (!g_up.load() && !start_radio()) {
    ESP_LOGW(TAG, "wake refused: the controller did not start");
    return;
  }
  // Re-advertise on every wake, not only on the first: a wake inside another
  // window (boot, a note, a link that just dropped) used to leave the radio up
  // and silent, which is a window that looks open and cannot be found.
  if (g_started) schedule(&g_update_ev);
  ESP_LOGI(TAG, "wake window open for %u ms, radio %s, %u paired of %u",
           static_cast<unsigned>(g_window.timing().trigger_ms), g_up.load() ? "up" : "starting", g_bonds.load(),
           kBondSlots);
}

void PhoneLink::set_pending(uint32_t count) { radio_set_pending(count); }

bool PhoneLink::radio_up() const { return g_up.load(); }

const char* PhoneLink::radio_reason() const { return hg::RadioWindow::name(g_window.why(now_ms())); }

// -- the note sync's radio side ------------------------------------------------

bool NoteLink::available() const { return g_started && g_note_ready.load(); }

bool NoteLink::connected() const { return g_started && g_conn.load() != kNone; }

uint16_t NoteLink::mtu() const { return g_note_mtu.load(); }

bool NoteLink::poll_command(std::string& out) {
  if (!g_started || !g_note_lines) return false;
  NoteLine line;
  if (xQueueReceive(g_note_lines, &line, 0) != pdTRUE) return false;
  out.assign(line.text, line.len);
  // A phone that sends CRLF is the same command as one that sends LF.
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
  return true;
}

void NoteLink::note_saved() { radio_note_saved(); }
void NoteLink::pending(uint32_t count) { radio_set_pending(count); }

bool NoteLink::send_frame(const uint8_t* data, size_t len) {
  const uint16_t conn = g_conn.load();
  if (!g_started || conn == kNone || !g_note_subscribed.load() || len == 0) return false;
  const uint16_t mtu = ble_att_mtu(conn);
  const size_t room = mtu > 3 ? static_cast<size_t>(mtu) - 3 : 20;
  if (len > room) return false;
  // ble_gatts_notify_custom frees the mbuf either way; ENOMEM here means the
  // host is out of buffers, and the caller offers the packet again next tick.
  os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
  if (!om) return false;
  return ble_gatts_notify_custom(conn, g_note_data_handle, om) == 0;
}

#endif  // CONFIG_BT_NIMBLE_ENABLED

}  // namespace hgp
