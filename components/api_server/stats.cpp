#include "stats.h"

#include "ble_backend.h"
#include "connection.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "proxy_config.h"
#include "scanner.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace api_server::stats {

namespace {

constexpr const char *TAG = "stats";

std::atomic<uint32_t> g_reads{0};
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_notifies{0};

#if CONFIG_NBP_WEB_CONSOLE
// ---- log ring buffer ----
//
// esp_log_set_vprintf installs a process-wide hook called from any
// task that does ESP_LOGx. We mirror each formatted line into a flat
// ring buffer indexed by a monotonic `g_log_seq` (total bytes ever
// written). `/log?since=N` returns the slice [N, g_log_seq). When the
// client falls behind by more than RING_SIZE, we resync from the
// oldest byte still resident.
//
// The hook keeps calling the original vprintf so UART/JTAG output is
// preserved — this is purely a tee.

// Bigger ring when NimBLE is compiled at DEBUG — DEBUG output during
// connect/discovery rolls a small ring in well under a second.
#if defined(CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG) || defined(CONFIG_NIMBLE_CPP_LOG_LEVEL_DEBUG)
constexpr size_t LOG_RING_SIZE = 65536;
#else
constexpr size_t LOG_RING_SIZE = 8192;
#endif
char g_log_ring[LOG_RING_SIZE];
uint32_t g_log_seq = 0;
SemaphoreHandle_t g_log_mutex = nullptr;
vprintf_like_t g_old_vprintf = nullptr;

void log_ring_append(const char *data, size_t len) {
  if (len == 0 || g_log_mutex == nullptr) return;
  // If a single line is bigger than the whole ring, keep only the tail.
  if (len > LOG_RING_SIZE) {
    data += len - LOG_RING_SIZE;
    len = LOG_RING_SIZE;
  }
  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  size_t pos = g_log_seq % LOG_RING_SIZE;
  size_t first = std::min(len, LOG_RING_SIZE - pos);
  std::memcpy(g_log_ring + pos, data, first);
  if (len > first) {
    std::memcpy(g_log_ring, data + first, len - first);
  }
  g_log_seq += len;
  xSemaphoreGive(g_log_mutex);
}

int log_vprintf(const char *fmt, va_list argptr) {
  // va_copy before consuming argptr in old_vprintf; vsnprintf into a
  // stack scratch — accept truncation for lines >sizeof(buf), no malloc
  // in a logging path.
  char buf[256];
  va_list ap;
  va_copy(ap, argptr);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t copy = (n >= static_cast<int>(sizeof(buf))) ? sizeof(buf) - 1
                                                       : static_cast<size_t>(n);
    log_ring_append(buf, copy);
  }
  return g_old_vprintf ? g_old_vprintf(fmt, argptr) : std::vprintf(fmt, argptr);
}

esp_err_t log_get(httpd_req_t *req) {
  uint32_t since = 0;
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[32];
    if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
      since = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
    }
  }

  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  uint32_t seq = g_log_seq;
  // If the client is way behind, clamp to the oldest byte we still hold.
  uint32_t backlog;
  if (since > seq) {
    // Counter wrap or client seq came from a previous boot; reset.
    since = seq;
    backlog = 0;
  } else if (seq - since > LOG_RING_SIZE) {
    since = seq - LOG_RING_SIZE;
    backlog = LOG_RING_SIZE;
  } else {
    backlog = seq - since;
  }

  char hdr[32];
  std::snprintf(hdr, sizeof(hdr), "%lu", static_cast<unsigned long>(seq));

  if (backlog == 0) {
    xSemaphoreGive(g_log_mutex);
    httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "", 0);
  }

  // Stream in two slices (handling wrap-around) via chunked encoding so
  // we never need a single contiguous malloc — fragmented heap with a
  // 64 KiB ring would otherwise fail the alloc and lose the response.
  // A small bounce buffer copies under the mutex; sends happen outside.
  size_t start = since % LOG_RING_SIZE;
  size_t first_len = std::min(static_cast<size_t>(backlog), LOG_RING_SIZE - start);
  size_t second_len = backlog - first_len;

  constexpr size_t CHUNK = 1024;
  static char bounce[CHUNK];

  httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  esp_err_t r = ESP_OK;
  auto send_range = [&](const char *base, size_t len) {
    while (len > 0 && r == ESP_OK) {
      size_t take = len < CHUNK ? len : CHUNK;
      std::memcpy(bounce, base, take);
      xSemaphoreGive(g_log_mutex);
      r = httpd_resp_send_chunk(req, bounce, take);
      xSemaphoreTake(g_log_mutex, portMAX_DELAY);
      base += take;
      len -= take;
    }
  };
  send_range(g_log_ring + start, first_len);
  if (r == ESP_OK && second_len > 0) {
    send_range(g_log_ring, second_len);
  }
  xSemaphoreGive(g_log_mutex);

  if (r == ESP_OK) {
    r = httpd_resp_send_chunk(req, nullptr, 0);  // terminator
  }
  return r;
}
#endif  // CONFIG_NBP_WEB_CONSOLE

// ---- NimBLE log-level override (NVS-persisted) ----
//
// One slot, one knob: a single esp_log_level applied to all the
// NimBLE-Cpp tags we know about. Stored in NVS namespace "stats" as
// key "nimble_lvl" (int8). Default = WARN, which is what we had
// hard-coded for NimBLEScan before.

constexpr const char *NVS_NS = "stats";
constexpr const char *NVS_LEVEL_KEY = "nimble_lvl";
constexpr esp_log_level_t DEFAULT_NIMBLE_LEVEL = ESP_LOG_WARN;

// Every NimBLE-Cpp log tag we've observed. Adding more is harmless;
// esp_log_level_set just stores the mapping. Split into "scan" and
// "core" so /trace can silence scanner noise while keeping host/client
// debug output flowing to the log ring.
constexpr const char *NIMBLE_SCAN_TAGS[] = {
    "NimBLEScan", "NimBLEAdvertisedDevice",
};
constexpr const char *NIMBLE_CORE_TAGS[] = {
    "NimBLE", "NimBLEDevice", "NimBLEClient",
    "NimBLERemoteCharacteristic",
};

esp_log_level_t g_current_nimble_level = DEFAULT_NIMBLE_LEVEL;

void apply_level(esp_log_level_t lvl) {
  for (const char *tag : NIMBLE_CORE_TAGS) esp_log_level_set(tag, lvl);
  // NimBLE-Cpp's scanner logs "New advertiser: <mac>" at INFO on every
  // advert with wantDuplicates=true — instantly floods the console at
  // INFO+. Cap scan tags at WARN regardless of the user-picked level;
  // they only become more verbose via /trace ON's explicit override.
  esp_log_level_t scan_lvl = (lvl < ESP_LOG_WARN) ? lvl : ESP_LOG_WARN;
  for (const char *tag : NIMBLE_SCAN_TAGS) esp_log_level_set(tag, scan_lvl);
  g_current_nimble_level = lvl;
}

#if CONFIG_NBP_WEB_CONSOLE
void log_ring_reset() {
  if (g_log_mutex == nullptr) return;
  xSemaphoreTake(g_log_mutex, portMAX_DELAY);
  g_log_seq = 0;
  xSemaphoreGive(g_log_mutex);
}
#endif

esp_err_t nvs_read_level(esp_log_level_t *out) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  int8_t v = 0;
  err = nvs_get_i8(h, NVS_LEVEL_KEY, &v);
  nvs_close(h);
  if (err != ESP_OK) return err;
  *out = static_cast<esp_log_level_t>(v);
  return ESP_OK;
}

esp_err_t nvs_write_level(esp_log_level_t lvl) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_i8(h, NVS_LEVEL_KEY, static_cast<int8_t>(lvl));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t level_get(httpd_req_t *req) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "{\"nimble\":%d}",
                        static_cast<int>(g_current_nimble_level));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

esp_err_t level_post(httpd_req_t *req) {
  // Accept the level in a query string: POST /level?nimble=2 .
  // Keeps the handler trivial — no body parsing needed.
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "missing query");
  }
  char val[8];
  if (httpd_query_key_value(query, "nimble", val, sizeof(val)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "missing nimble=");
  }
  int parsed = std::atoi(val);
  if (parsed < ESP_LOG_NONE || parsed > ESP_LOG_VERBOSE) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "level out of range");
  }
  auto lvl = static_cast<esp_log_level_t>(parsed);
  esp_err_t err = nvs_write_level(lvl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs write failed: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "nvs write failed");
  }
  apply_level(lvl);
  ESP_LOGI(TAG, "NimBLE log level set to %d (persisted)",
           static_cast<int>(lvl));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}


// Diagnostic capture mode. /trace?on=1 silences scanner noise and
// pauses scanning so the 64 KiB log ring isn't flooded with "New
// advertiser" lines during a BMS bring-up, then resets log_seq so the
// next `/log?since=0` returns a clean trace starting after `on=1`.
// /trace?on=0 restores the persisted NimBLE level and resumes scanning.
esp_err_t trace_post(httpd_req_t *req) {
  char query[32];
  char val[8];
  bool on = true;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK) {
    on = (std::atoi(val) != 0);
  }
  if (on) {
    for (const char *tag : NIMBLE_SCAN_TAGS) esp_log_level_set(tag, ESP_LOG_ERROR);
    for (const char *tag : NIMBLE_CORE_TAGS) esp_log_level_set(tag, ESP_LOG_DEBUG);
    ble_backend::scanner::pause();
#if CONFIG_NBP_WEB_CONSOLE
    log_ring_reset();
#endif
    ESP_LOGI(TAG, "trace ON: scan paused, core=DEBUG, scan-tags=ERROR");
  } else {
    apply_level(g_current_nimble_level);
    ble_backend::scanner::resume();
    ESP_LOGI(TAG, "trace OFF: scan resumed, levels restored to %d",
             static_cast<int>(g_current_nimble_level));
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, on ? "{\"trace\":true}" : "{\"trace\":false}",
                         HTTPD_RESP_USE_STRLEN);
}

esp_err_t reboot_post(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "rebooting\n", HTTPD_RESP_USE_STRLEN);
  ESP_LOGI(TAG, "reboot requested via /reboot");
  // Let the response drain and the TCP FIN reach the client before we
  // yank the rug — same pattern as the OTA handler.
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

esp_err_t stats_get(httpd_req_t *req) {
  char buf[192];
  unsigned in_use = proxy::MAX_CONNECTIONS -
                    ble_backend::connection::free_slots();
  int n = std::snprintf(
      buf, sizeof(buf),
      "{\"reads\":%lu,\"writes\":%lu,\"notifies\":%lu,\"adverts\":%lu,"
      "\"connections\":%u,\"heap\":%lu,"
      "\"notify_rx\":%lu,\"last_notify_handle\":%u}",
      static_cast<unsigned long>(g_reads.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_writes.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_notifies.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(ble_backend::scanner::adv_count()),
      in_use,
      static_cast<unsigned long>(esp_get_free_heap_size()),
      static_cast<unsigned long>(ble_backend::notify_rx_total()),
      ble_backend::last_notify_handle());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

#if CONFIG_NBP_DEVICES_PANEL
esp_err_t devices_get(httpd_req_t *req) {
  // Snapshot under the scanner mutex, then format outside it.
  static ble_backend::scanner::DeviceRow snap[64];
  size_t n = ble_backend::scanner::snapshot_devices(snap, 64);
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  // httpd serializes requests on a single worker, so a static buffer
  // is safe and avoids putting 6 KiB on the worker stack.
  static char out[6144];
  char *p = out;
  char *const end = out + sizeof(out);
  auto rem = [&]() -> size_t { return end > p ? size_t(end - p) : 0; };
  auto bump = [&](int w) {
    if (w > 0) p += size_t(w) < rem() ? size_t(w) : rem();
  };

  bump(std::snprintf(p, rem(), "{\"devices\":["));
  for (size_t i = 0; i < n; ++i) {
    const auto &r = snap[i];
    uint8_t b[6];
    for (int k = 0; k < 6; ++k) b[k] = (r.addr >> ((5 - k) * 8)) & 0xff;
    bump(std::snprintf(
        p, rem(),
        "%s{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"type\":%u,\"rssi\":%d,\"count\":%lu,\"age\":%lu,\"name\":\"",
        i ? "," : "", b[0], b[1], b[2], b[3], b[4], b[5],
        static_cast<unsigned>(r.addr_type), static_cast<int>(r.rssi),
        static_cast<unsigned long>(r.adv_count),
        static_cast<unsigned long>(now_ms - r.last_ms)));
    // JSON-escape the name: backslash " and \, \u-escape controls.
    for (const char *q = r.name; *q && rem() > 8; ++q) {
      char c = *q;
      if (c == '"' || c == '\\') {
        *p++ = '\\'; *p++ = c;
      } else if (static_cast<unsigned char>(c) < 0x20) {
        bump(std::snprintf(p, rem(), "\\u%04x",
                           static_cast<unsigned>(static_cast<unsigned char>(c))));
      } else {
        *p++ = c;
      }
    }
    if (rem() >= 2) { *p++ = '"'; *p++ = '}'; }
  }
  if (rem() >= 2) { *p++ = ']'; *p++ = '}'; }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, p - out);
}
#endif  // CONFIG_NBP_DEVICES_PANEL

esp_err_t root_get(httpd_req_t *req) {
  // uPlot loaded from jsdelivr. Page polls /stats.json each second and
  // plots the per-second delta over a 120-sample (2 min) window.
  static const char page[] =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<title>nimble-ble-proxy</title>"
      "<link rel=stylesheet "
      "href=\"https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/uPlot.min.css\">"
      "<style>"
      "body{font:14px system-ui;margin:1em;color:#eee;background:#111}"
      "h1,h2{font-size:1.1em;margin:0 0 1em}"
      "h2{margin-top:1.5em}"
      "#chart{background:#1a1a1a;padding:.5em;border-radius:6px;"
      "display:inline-block}"
#if CONFIG_NBP_WEB_CONSOLE
      "#console{background:#0a0a0a;color:#d4d4d4;"
      "font:11px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
      "padding:.5em;border-radius:6px;width:900px;height:300px;"
      "overflow-y:auto;white-space:pre-wrap;word-break:break-all;"
      "margin:0;border:1px solid #222}"
#endif
      "footer{margin-top:1em;color:#888;font-size:.85em}"
      "code{color:#bbb}"
      "#controls{margin:1em 0;display:flex;gap:1em;align-items:center}"
      "#controls label{color:#aaa}"
      "#controls select,#controls button{"
      "background:#1a1a1a;color:#eee;border:1px solid #333;"
      "border-radius:4px;padding:.3em .6em;font:inherit;cursor:pointer}"
      "#controls button:hover{background:#2a2a2a}"
      "#controls button.danger{border-color:#7f1d1d;color:#fca5a5}"
      "#controls button.danger:hover{background:#3b0a0a}"
#if CONFIG_NBP_DEVICES_PANEL
      "table#devices{border-collapse:collapse;font-size:12px;color:#ddd;"
      "margin-bottom:1em}"
      "table#devices th,table#devices td{padding:.2em .6em;"
      "border-bottom:1px solid #222;text-align:left}"
      "table#devices th{color:#888;font-weight:normal}"
      "table#devices .r{text-align:right;font-variant-numeric:tabular-nums}"
      "table#devices tr.stale{opacity:.4}"
#endif
      "</style></head><body>"
      "<h1>nimble-ble-proxy &mdash; BLE activity/s</h1>"
      "<div id=chart></div>"
#if CONFIG_NBP_DEVICES_PANEL
      "<h2>devices seen</h2>"
      "<table id=devices><thead><tr>"
      "<th>MAC</th><th>name</th><th class=r>RSSI</th>"
      "<th class=r>adv/s</th><th class=r>total</th><th class=r>age</th>"
      "</tr></thead><tbody></tbody></table>"
#endif
      "<div id=controls>"
      "<label>NimBLE log level: "
      "<select id=lvl>"
      "<option value=0>NONE</option>"
      "<option value=1>ERROR</option>"
      "<option value=2>WARN</option>"
      "<option value=3>INFO</option>"
      "<option value=4>DEBUG</option>"
      "<option value=5>VERBOSE</option>"
      "</select></label>"
      "<button id=reboot class=danger>reboot device</button>"
      "</div>"
#if CONFIG_NBP_WEB_CONSOLE
      "<h2>device log</h2>"
      "<pre id=console></pre>"
#endif
      "<footer>OTA: <code>curl --data-binary @firmware.bin "
      "http://&lt;host&gt;/update</code></footer>"
      "<script src=\"https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/"
      "uPlot.iife.min.js\"></script>"
#if CONFIG_NBP_WEB_CONSOLE
      "<script src=\"https://cdn.jsdelivr.net/npm/ansi_up@5/ansi_up.js\">"
      "</script>"
#endif
      "<script>"
      "const N=120,t=[],r=[],w=[],n=[],a=[],c=[],h=[];"
      "for(let i=0;i<N;i++){t.push(i-N+1);r.push(null);w.push(null);"
      "n.push(null);a.push(null);c.push(null);h.push(null);}"
      "const fmt1=(u,v)=>v==null?'--':v.toFixed(1);"
      "const u=new uPlot({width:900,height:320,"
      "scales:{x:{time:false},y:{},kb:{}},"
      "axes:[{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{side:1,scale:'kb',stroke:'#9ca3af',grid:{show:false},"
      "values:(u,vs)=>vs.map(v=>v+' KB')}],"
      "series:[{label:'t (s ago)'},"
      "{label:'reads/s',stroke:'#4ade80',width:2,value:fmt1},"
      "{label:'writes/s',stroke:'#60a5fa',width:2,value:fmt1},"
      "{label:'notifies/s',stroke:'#f472b6',width:2,value:fmt1},"
      "{label:'adverts/s',stroke:'#fbbf24',width:2,value:fmt1},"
      "{label:'conns',stroke:'#a78bfa',width:2},"
      "{label:'heap',scale:'kb',stroke:'#9ca3af',width:2,dash:[4,4]}]},"
      "[t,r,w,n,a,c,h],document.getElementById('chart'));"
      "let prev=null,prevT=null;"
      "const d=(cur,p,dt)=>{const v=(cur-p)/dt;return v<0?null:v;};"
      "async function tick(){"
      "try{const now=performance.now()/1000;"
      "const s=await(await fetch('/stats.json')).json();"
      "if(prev){const dt=now-prevT;"
      "r.shift();w.shift();n.shift();a.shift();c.shift();h.shift();"
      "r.push(d(s.reads,prev.reads,dt));"
      "w.push(d(s.writes,prev.writes,dt));"
      "n.push(d(s.notifies,prev.notifies,dt));"
      "a.push(d(s.adverts,prev.adverts,dt));"
      "c.push(s.connections);"
      "h.push(Math.round(s.heap/1024));"
      "u.setData([t,r,w,n,a,c,h]);}"
      "prev=s;prevT=now;}catch(e){}}"
      "setInterval(tick,1000);tick();"
#if CONFIG_NBP_WEB_CONSOLE
      "let logSeq=0;const con=document.getElementById('console');"
      // ansi_up output is HTML-safe (input is escape_html'd then wrapped
      // in <span>s); we still avoid innerHTML by inserting via a Range
      // fragment and trim by removing leading child nodes.
      "const au=new AnsiUp();au.use_classes=false;"
      "async function pollLog(){"
      "try{const r=await fetch('/log?since='+logSeq);"
      "logSeq=parseInt(r.headers.get('X-Log-Seq')||logSeq,10);"
      "const txt=await r.text();"
      "if(txt){"
      "const atBottom=con.scrollHeight-con.scrollTop-con.clientHeight<20;"
      "const frag=document.createRange()"
      ".createContextualFragment(au.ansi_to_html(txt));"
      "con.appendChild(frag);"
      "while(con.childNodes.length>3000)con.removeChild(con.firstChild);"
      "if(atBottom)con.scrollTop=con.scrollHeight;"
      "}}catch(e){}}"
      "setInterval(pollLog,500);pollLog();"
#endif
      "const lvl=document.getElementById('lvl');"
      "fetch('/level').then(r=>r.json()).then(j=>{lvl.value=j.nimble;});"
      "lvl.onchange=()=>{"
      "fetch('/level?nimble='+lvl.value,{method:'POST'})"
      ".then(r=>{if(!r.ok)alert('level update failed');});};"
      "document.getElementById('reboot').onclick=()=>{"
      "if(!confirm('Reboot device?'))return;"
      "fetch('/reboot',{method:'POST'})"
#if CONFIG_NBP_WEB_CONSOLE
      ".then(()=>{con.appendChild(document.createTextNode("
      "'\\n[client] reboot requested, waiting for device...\\n'));})"
#endif
      ";};"
#if CONFIG_NBP_DEVICES_PANEL
      // Per-device adv/s computed from delta of `count` between polls,
      // same pattern as the global rates. devPrev gets rebuilt each
      // tick so it can't grow unbounded as the LRU evicts entries.
      "let devPrev={};"
      "async function pollDevices(){try{"
      "const d=(await(await fetch('/devices')).json()).devices;"
      "const now=performance.now()/1000;const next={};"
      "d.sort((a,b)=>b.count-a.count);"
      "const tb=document.querySelector('#devices tbody');tb.textContent='';"
      "for(const x of d){const p=devPrev[x.addr];let rate='--';"
      "if(p){const dt=now-p.t;if(dt>0){const v=(x.count-p.count)/dt;"
      "if(v>=0)rate=v.toFixed(1);}}"
      "next[x.addr]={count:x.count,t:now};"
      "const tr=document.createElement('tr');"
      "if(x.age>10000)tr.className='stale';"
      "const cells=[[x.addr,''],[x.name||'',''],"
      "[x.rssi,'r'],[rate,'r'],[x.count,'r'],"
      "[(x.age/1000).toFixed(1)+'s','r']];"
      "for(const [v,cls] of cells){const td=document.createElement('td');"
      "if(cls)td.className=cls;td.textContent=v;tr.appendChild(td);}"
      "tb.appendChild(tr);}devPrev=next;}catch(e){}}"
      "setInterval(pollDevices,1000);pollDevices();"
#endif
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, sizeof(page) - 1);
}

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }

void apply_log_overrides_from_nvs() {
  esp_log_level_t lvl;
  if (nvs_read_level(&lvl) == ESP_OK) {
    apply_level(lvl);
    ESP_LOGI(TAG, "NimBLE log level from NVS: %d", static_cast<int>(lvl));
  } else {
    apply_level(DEFAULT_NIMBLE_LEVEL);
    ESP_LOGI(TAG, "NimBLE log level default: %d",
             static_cast<int>(DEFAULT_NIMBLE_LEVEL));
  }
}

#if CONFIG_NBP_WEB_CONSOLE
void install_log_hook() {
  if (g_log_mutex != nullptr) return;  // already installed
  g_log_mutex = xSemaphoreCreateMutex();
  g_old_vprintf = esp_log_set_vprintf(&log_vprintf);
  if (g_old_vprintf == nullptr) g_old_vprintf = &std::vprintf;
  ESP_LOGI(TAG, "log hook installed, %u-byte ring",
           static_cast<unsigned>(LOG_RING_SIZE));
}
#endif

void register_endpoints(httpd_handle_t srv) {
  if (srv == nullptr) {
    ESP_LOGW(TAG, "no httpd handle, stats UI disabled");
    return;
  }
  httpd_uri_t root = {.uri = "/",
                      .method = HTTP_GET,
                      .handler = &root_get,
                      .user_ctx = nullptr};
  httpd_uri_t stats = {.uri = "/stats.json",
                       .method = HTTP_GET,
                       .handler = &stats_get,
                       .user_ctx = nullptr};
#if CONFIG_NBP_WEB_CONSOLE
  httpd_uri_t log = {.uri = "/log",
                     .method = HTTP_GET,
                     .handler = &log_get,
                     .user_ctx = nullptr};
#endif
  httpd_uri_t level_g = {.uri = "/level",
                         .method = HTTP_GET,
                         .handler = &level_get,
                         .user_ctx = nullptr};
  httpd_uri_t level_p = {.uri = "/level",
                         .method = HTTP_POST,
                         .handler = &level_post,
                         .user_ctx = nullptr};
  httpd_uri_t reboot = {.uri = "/reboot",
                        .method = HTTP_POST,
                        .handler = &reboot_post,
                        .user_ctx = nullptr};
  httpd_uri_t trace = {.uri = "/trace",
                       .method = HTTP_POST,
                       .handler = &trace_post,
                       .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &stats);
#if CONFIG_NBP_WEB_CONSOLE
  httpd_register_uri_handler(srv, &log);
#endif
  httpd_register_uri_handler(srv, &level_g);
  httpd_register_uri_handler(srv, &level_p);
  httpd_register_uri_handler(srv, &reboot);
  httpd_register_uri_handler(srv, &trace);
#if CONFIG_NBP_DEVICES_PANEL
  httpd_uri_t devices = {.uri = "/devices",
                         .method = HTTP_GET,
                         .handler = &devices_get,
                         .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &devices);
#endif
  ESP_LOGI(TAG, "stats UI at /");
}

}  // namespace api_server::stats
