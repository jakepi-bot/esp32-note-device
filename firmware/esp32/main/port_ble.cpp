// Phone companion link over Bluetooth LE (NimBLE). See PhoneLink in port.hpp and
// docs/protocol.md#phone-app-over-bluetooth.
//
// Threads: NimBLE's host task runs every GAP and GATT callback, and every call
// here that changes advertising or the bond store is moved onto it with an
// event. A worker task hands command lines to the app task as console requests
// (like the serial console) and notifies the replies.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_random.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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
void PhoneLink::close_pairing() {}
void PhoneLink::forget() {}
void PhoneLink::set_name(const std::string&) {}

// The note sync lives on the same stack; without NimBLE there is nothing to
// register and the notes mode never asks.
bool NoteLink::available() const { return false; }
bool NoteLink::connected() const { return false; }
uint16_t NoteLink::mtu() const { return 23; }
bool NoteLink::poll_command(std::string&) { return false; }
bool NoteLink::send_frame(const uint8_t*, size_t) { return false; }

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
std::atomic<uint16_t> g_conn{kNone};
uint16_t g_pairing_conn = kNone;  // host task only: the connection shown a passkey
uint8_t g_own_addr_type = 0;
std::mutex g_name_mutex;
std::string g_name;

ble_npl_event g_update_ev, g_forget_ev;
ble_npl_callout g_drop;

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

void post(PhoneSample::Kind kind, uint32_t passkey = 0) {
  PhoneSample s{kind, passkey, 0, false};
  if (kind == PhoneSample::Kind::Status) {
    s.paired = static_cast<uint8_t>(bonds());
    s.connected = g_conn.load() != kNone;
  }
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
  fields.uuids128 = &g_service_uuid;
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

void on_forget(ble_npl_event*) {
  ble_store_clear();
  ESP_LOGI(TAG, "paired phones removed");
  post(PhoneSample::Kind::Status);
  if (g_conn.load() != kNone) ble_npl_callout_reset(&g_drop, ble_npl_time_ms_to_ticks32(kForgetDropMs));
  else advertise();
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
      g_note_subscribed = false;
      g_note_mtu = ble_att_mtu(event->connect.conn_handle);
      ESP_LOGI(TAG, "phone connected");
      post(PhoneSample::Kind::Status);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      g_conn = kNone;
      g_note_subscribed = false;
      g_note_mtu = 23;
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
      ble_sm_io io = {};
      io.action = BLE_SM_IOACT_DISP;
      io.passkey = esp_random() % 1000000;
      g_pairing_conn = conn;
      ble_sm_inject_io(conn, &io);
      post(PhoneSample::Kind::Passkey, io.passkey);
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
          ESP_LOGI(TAG, "phone paired");
          post(PhoneSample::Kind::Paired);
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
  g_note_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  g_note_chrs[1].val_handle = &g_note_data_handle;
  g_note_chrs[2] = {};
  g_svcs[1] = {};
  g_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_svcs[1].uuid = &g_note_service_uuid.u;
  g_svcs[1].characteristics = g_note_chrs;
  g_svcs[2] = {};

  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(g_svcs);
  if (rc == 0) rc = ble_gatts_add_svcs(g_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "can't register the phone service (%d)", rc);
    return false;
  }
  g_note_ready = true;
  ble_store_config_init();
  ble_npl_event_init(&g_update_ev, on_update, nullptr);
  ble_npl_event_init(&g_forget_ev, on_forget, nullptr);
  ble_npl_callout_init(&g_drop, nimble_port_get_dflt_eventq(), on_drop, nullptr);
  nimble_port_freertos_init(host_task);
  g_started = true;
  ESP_LOGI(TAG, "Bluetooth ready");
  return true;
}

bool PhoneLink::open_pairing() {
  if (!g_started || !g_synced) return false;
  g_pairing = true;
  ESP_LOGI(TAG, "pairing open");
  schedule(&g_update_ev);
  return true;
}

void PhoneLink::close_pairing() {
  if (!g_started || !g_pairing.exchange(false)) return;
  ESP_LOGI(TAG, "pairing closed");
  schedule(&g_update_ev);
}

void PhoneLink::forget() {
  if (g_started) schedule(&g_forget_ev);
}

void PhoneLink::set_name(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    g_name = name;
  }
  if (g_started) schedule(&g_update_ev);
}

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
