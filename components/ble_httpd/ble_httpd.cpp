#include "ble_httpd.h"

#ifdef CONFIG_NBP_BLE_HTTPD

#include "NimBLEAdvertising.h"
#include "NimBLECharacteristic.h"
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLEService.h"
#include "NimBLEUUID.h"
#include "ble_backend.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scanner.h"
#include "stats.h"

#if CONFIG_NBP_CLONE
#include "clone.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>

namespace ble_httpd {

namespace {

constexpr const char *TAG = "ble_httpd";

// 128-bit UUIDs. Service base 6e627062-7072-7879-0001-0000_0000_0000;
// the last two bytes pick the characteristic (00=service, 01=REQUEST,
// 02=RESPONSE, 03=INFO).
constexpr const char *SVC_UUID  = "6e627062-7072-7879-0001-000000000000";
constexpr const char *REQ_UUID  = "6e627062-7072-7879-0001-000000000001";
constexpr const char *RESP_UUID = "6e627062-7072-7879-0001-000000000002";
constexpr const char *INFO_UUID = "6e627062-7072-7879-0001-000000000003";

constexpr uint8_t FRAME_FIN = 0x01;
constexpr uint8_t FRAME_ERR = 0x02;

constexpr uint8_t PROTO_VERSION = 1;

std::atomic<bool> g_started{false};
std::atomic<bool> g_activated{false};
NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_resp_chr = nullptr;

// One response buffer reused across requests. Single connection
// serialises one in-flight request — fine for a dashboard. 4 KB fits
// /devices for ~30 entries (well above the scanner's 64-row cap of
// useful data given typical 120 bytes per device JSON).
constexpr size_t RESP_BUF_BYTES = 4096;
char g_resp_buf[RESP_BUF_BYTES];

void send_response(uint8_t reqId, const char *payload, size_t len,
                   bool is_error) {
  if (g_resp_chr == nullptr) return;

  // Fragment into MTU-3 chunks; subtract 2 more for our header.
  // NimBLE-CPP's getMTU reports the current per-conn negotiated MTU;
  // 23 is the safe floor before any exchange. We use the device-wide
  // setMTU(247) as the ceiling.
  constexpr size_t HDR = 2;
  constexpr size_t MAX_FRAG = 247 - 3 - HDR;  // 242 payload bytes/frame

  size_t off = 0;
  uint8_t frame[2 + MAX_FRAG];
  while (true) {
    size_t take = len - off;
    bool last = take <= MAX_FRAG;
    if (!last) take = MAX_FRAG;
    frame[0] = (last ? FRAME_FIN : 0) | (is_error ? FRAME_ERR : 0);
    frame[1] = reqId;
    std::memcpy(frame + 2, payload + off, take);
    g_resp_chr->setValue(frame, take + 2);

    // Retry on transient failure: NimBLE drops notify() silently when
    // the outbound MBuf pool is empty, which the client sees as a
    // forever-hanging request (FIN bit never arrives). A short delay
    // lets the host task recycle MBufs.
    bool ok = false;
    for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
      ok = g_resp_chr->notify();
      if (!ok) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ok) {
      ESP_LOGW(TAG, "notify dropped at off=%u/%u; client will retry",
               static_cast<unsigned>(off), static_cast<unsigned>(len));
      break;  // give up; client hits its own timeout and retries
    }

    off += take;
    if (last) break;
    // No per-fragment delay: the retry-on-fail path above handles the
    // (rare) MBuf-exhaustion case. vTaskDelay(1) here added 10 ms per
    // fragment (HZ=100), making a 17-frag /log take 170 ms.
  }
}

// Parse a single `key=value` from a query string. Returns 0 on success.
// `query` is the substring after '?'. `out` is a small numeric scratch.
bool find_query_u32(const char *query, const char *key, uint32_t *out) {
  size_t klen = std::strlen(key);
  const char *p = query;
  while (p && *p) {
    if (std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
      *out = static_cast<uint32_t>(std::strtoul(p + klen + 1, nullptr, 10));
      return true;
    }
    const char *amp = std::strchr(p, '&');
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

// Render either {"ok":true} or {"error":"<msg>"} into `out`. `err` is
// the return value from a handle_*_set helper (nullptr on success).
size_t render_ok_or_error(char *out, size_t cap, const char *err) {
  if (err == nullptr) {
    return static_cast<size_t>(std::snprintf(out, cap, "{\"ok\":true}"));
  }
  return static_cast<size_t>(
      std::snprintf(out, cap, "{\"error\":\"%s\"}", err));
}

size_t dispatch(const char *method, char *path, char *out, size_t cap) {
  // Split off query string at '?'. Leaves path with NUL where '?' was;
  // path is the caller's mutable scratch buffer, so this is fine.
  char *qmark = std::strchr(path, '?');
  const char *query = "";
  if (qmark) { *qmark = '\0'; query = qmark + 1; }

  const bool is_get = std::strcmp(method, "GET") == 0;
  const bool is_post = std::strcmp(method, "POST") == 0;

  if (is_get && std::strcmp(path, "/stats.json") == 0) {
    return api_server::stats::build_stats_json(out, cap);
  }

#if CONFIG_NBP_WEB_CONSOLE
  if (is_get && std::strcmp(path, "/log") == 0) {
    // Response payload: "<next_since>\n<log bytes>".
    // Fixed-width 10-digit decimal + newline = 11-byte header so we can
    // reserve space and back-fill without shifting the log content.
    constexpr size_t PREFIX = 11;
    if (cap <= PREFIX) return 0;
    // Cap per-request payload at ~1 KB (≤5 BLE fragments) regardless of
    // the response-buffer size — keeps polling latency tight even when
    // the client just connected and is draining a full ring. The
    // remainder is delivered on subsequent /log polls (500 ms tick).
    constexpr size_t LOG_MAX = 1024;
    size_t log_cap = cap - PREFIX;
    if (log_cap > LOG_MAX) log_cap = LOG_MAX;
    uint32_t since = 0;
    find_query_u32(query, "since", &since);
    uint32_t seq = 0;
    size_t n = api_server::stats::build_log_slice(
        &since, out + PREFIX, log_cap, &seq);
    uint32_t next_since = since + n;
    char hdr[16];
    int plen = std::snprintf(hdr, sizeof(hdr), "%010lu\n",
                             static_cast<unsigned long>(next_since));
    if (plen != static_cast<int>(PREFIX)) return 0;
    std::memcpy(out, hdr, PREFIX);
    return PREFIX + n;
  }
#endif

  if (is_get && std::strcmp(path, "/level") == 0) {
    return api_server::stats::build_level_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/level") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_level_set(query));
  }

  if (is_get && std::strcmp(path, "/txpower") == 0) {
    return api_server::stats::build_txpower_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/txpower") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_txpower_set(query));
  }

  if (is_get && std::strcmp(path, "/cpufreq") == 0) {
    return api_server::stats::build_cpufreq_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/cpufreq") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_cpufreq_set(query));
  }

  if (is_get && std::strcmp(path, "/advitvl") == 0) {
    return api_server::stats::build_advitvl_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/advitvl") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_advitvl_set(query));
  }

  if (is_get && std::strcmp(path, "/wifips") == 0) {
    return api_server::stats::build_wifips_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/wifips") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_wifips_set(query));
  }

  if (is_get && std::strcmp(path, "/hostname") == 0) {
    return api_server::stats::build_hostname_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/hostname") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_hostname_set(query));
  }

  if (is_get && std::strcmp(path, "/scan") == 0) {
    return api_server::stats::build_scan_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/scan") == 0) {
    return render_ok_or_error(out, cap,
                              api_server::stats::handle_scan_set(query));
  }


#if CONFIG_NBP_DEVICES_PANEL
  if (is_get && std::strcmp(path, "/devices") == 0) {
    return api_server::stats::build_devices_json(out, cap);
  }
#endif

  if (is_post && std::strcmp(path, "/trace") == 0) {
    bool on = api_server::stats::handle_trace_set(query);
    return static_cast<size_t>(std::snprintf(
        out, cap, on ? "{\"trace\":true}" : "{\"trace\":false}"));
  }

  if (is_post && std::strcmp(path, "/reboot") == 0) {
    api_server::stats::schedule_reboot();
    return static_cast<size_t>(std::snprintf(out, cap, "{\"ok\":true}"));
  }

#if CONFIG_NBP_CLONE
  if (is_get && std::strcmp(path, "/clone") == 0) {
    return ble_clone::build_clone_json(out, cap);
  }
  if (is_post && std::strcmp(path, "/clone") == 0) {
    bool reboot = false;
    const char *err = ble_clone::handle_clone_set(query, &reboot);
    if (err) {
      return static_cast<size_t>(
          std::snprintf(out, cap, "{\"error\":\"%s\"}", err));
    }
    return static_cast<size_t>(std::snprintf(
        out, cap, "{\"ok\":true,\"reboot_required\":%s}",
        reboot ? "true" : "false"));
  }
#endif

  // Unknown path: JSON-format so apiJson on the client can parse + throw,
  // instead of choking on bare text. apiText callers see the same body.
  return static_cast<size_t>(
      std::snprintf(out, cap, "{\"error\":\"no route %s\"}", path));
}

class ReqCb : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    auto val = chr->getValue();
    const uint8_t *bytes = val.data();
    size_t n = val.size();
    if (n < 2) {
      ESP_LOGW(TAG, "short request frame: %u byte(s)",
               static_cast<unsigned>(n));
      return;
    }
    uint8_t flags = bytes[0];
    uint8_t reqId = bytes[1];
    if (!(flags & FRAME_FIN)) {
      ESP_LOGW(TAG, "multi-fragment request not yet supported");
      send_response(reqId, "fragmented requests unsupported", 31, true);
      return;
    }

    // Payload format: "<METHOD> <PATH>" — NUL-terminate in a scratch.
    char line[256];
    size_t payload_len = n - 2;
    if (payload_len >= sizeof(line)) payload_len = sizeof(line) - 1;
    std::memcpy(line, bytes + 2, payload_len);
    line[payload_len] = '\0';

    char *space = std::strchr(line, ' ');
    if (space == nullptr) {
      send_response(reqId, "bad request", 11, true);
      return;
    }
    *space = '\0';
    const char *method = line;
    char *path = space + 1;

    size_t out_len = dispatch(method, path, g_resp_buf, sizeof(g_resp_buf));
    if (out_len > sizeof(g_resp_buf)) out_len = sizeof(g_resp_buf);
    send_response(reqId, g_resp_buf, out_len, false);
  }
};
ReqCb g_req_cb;

class ServerCb : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    ESP_LOGI(TAG, "central connected");
    // Keep advertising so a dropped client can reconnect cleanly, unless
    // the peripheral-advertising master switch is off.
    auto *adv = NimBLEDevice::getAdvertising();
    if (adv != nullptr && ble_backend::advertising_enabled()) adv->start();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    ESP_LOGI(TAG, "central disconnected reason=%d", reason);
    auto *adv = NimBLEDevice::getAdvertising();
    if (adv != nullptr && ble_backend::advertising_enabled()) adv->start();
  }
};
ServerCb g_server_cb;

}  // namespace

void start() {
  if (g_started.exchange(true)) return;

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_server_cb, /*deleteCallbacks=*/false);

  NimBLEService *svc = g_server->createService(NimBLEUUID(SVC_UUID));
  if (svc == nullptr) {
    ESP_LOGE(TAG, "createService failed");
    return;
  }

  NimBLECharacteristic *req_chr = svc->createCharacteristic(
      NimBLEUUID(REQ_UUID), NIMBLE_PROPERTY::WRITE);
  req_chr->setCallbacks(&g_req_cb);

  g_resp_chr = svc->createCharacteristic(
      NimBLEUUID(RESP_UUID), NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic *info_chr = svc->createCharacteristic(
      NimBLEUUID(INFO_UUID), NIMBLE_PROPERTY::READ);
  // INFO payload: [u8 version][u8 reserved][u16 max_frag_bytes_le].
  uint8_t info_val[4] = {PROTO_VERSION, 0, 242, 0};
  info_chr->setValue(info_val, sizeof(info_val));

  ESP_LOGI(TAG, "ble_httpd: services created (svc=%s); awaiting activate",
           SVC_UUID);
}

void activate() {
  if (g_activated.exchange(true)) return;
  if (g_server == nullptr) {
    ESP_LOGE(TAG, "activate called before start");
    return;
  }

  // NimBLE refuses GATT mutations (ble_gatts_add_svcs → BLE_HS_EBUSY)
  // while any GAP procedure is active. ble_backend::start() leaves the
  // scanner running, so pause it across server->start(), then resume.
  // NimBLEService::start() is a deprecated no-op in NimBLE-CPP 2.5 —
  // the real registration happens inside NimBLEServer::start(). This
  // is the *single* server->start() per session: ble_clone has added
  // its services to m_svcVec by now, so all of them register together
  // and gattRegisterCallback assigns handles to both sets.
  ble_backend::scanner::pause();
  bool ok = g_server->start();
  ble_backend::scanner::resume();
  if (!ok) {
    ESP_LOGE(TAG, "server start failed");
    return;
  }

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NimBLEUUID(SVC_UUID));
  adv->enableScanResponse(true);
  // Configure the payload either way so re-enabling at runtime resumes
  // cleanly, but only begin broadcasting if the master switch is on.
  if (ble_backend::advertising_enabled()) {
    adv->start();
  } else {
    ESP_LOGI(TAG, "advertising disabled (POST /advitvl?ms=0 to enable)");
  }

  ESP_LOGI(TAG, "ble_httpd activated: svc=%s", SVC_UUID);
}

}  // namespace ble_httpd

#endif  // CONFIG_NBP_BLE_HTTPD
