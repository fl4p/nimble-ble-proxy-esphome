#include "stats.h"

#include "connection.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

constexpr size_t LOG_RING_SIZE = 8192;
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

  // Copy out under the mutex; send outside so the lock isn't held
  // across socket IO.
  char *out = static_cast<char *>(std::malloc(backlog));
  if (out == nullptr) {
    xSemaphoreGive(g_log_mutex);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "log alloc failed");
  }
  size_t start = since % LOG_RING_SIZE;
  size_t first = std::min(static_cast<size_t>(backlog), LOG_RING_SIZE - start);
  std::memcpy(out, g_log_ring + start, first);
  if (backlog > first) {
    std::memcpy(out + first, g_log_ring, backlog - first);
  }
  xSemaphoreGive(g_log_mutex);

  httpd_resp_set_hdr(req, "X-Log-Seq", hdr);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  esp_err_t r = httpd_resp_send(req, out, backlog);
  std::free(out);
  return r;
}

esp_err_t stats_get(httpd_req_t *req) {
  char buf[192];
  unsigned in_use = proxy::MAX_CONNECTIONS -
                    ble_backend::connection::free_slots();
  int n = std::snprintf(
      buf, sizeof(buf),
      "{\"reads\":%lu,\"writes\":%lu,\"notifies\":%lu,\"adverts\":%lu,"
      "\"connections\":%u,\"heap\":%lu}",
      static_cast<unsigned long>(g_reads.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_writes.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(g_notifies.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(ble_backend::scanner::adv_count()),
      in_use,
      static_cast<unsigned long>(esp_get_free_heap_size()));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

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
      "#console{background:#0a0a0a;color:#d4d4d4;"
      "font:11px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
      "padding:.5em;border-radius:6px;width:900px;height:300px;"
      "overflow-y:auto;white-space:pre-wrap;word-break:break-all;"
      "margin:0;border:1px solid #222}"
      "footer{margin-top:1em;color:#888;font-size:.85em}"
      "code{color:#bbb}"
      "</style></head><body>"
      "<h1>nimble-ble-proxy &mdash; BLE activity/s</h1>"
      "<div id=chart></div>"
      "<h2>device log</h2>"
      "<pre id=console></pre>"
      "<footer>OTA: <code>curl --data-binary @firmware.bin "
      "http://&lt;host&gt;/update</code></footer>"
      "<script src=\"https://cdn.jsdelivr.net/npm/uplot@1.6.31/dist/"
      "uPlot.iife.min.js\"></script>"
      "<script>"
      "const N=120,t=[],r=[],w=[],n=[],a=[],c=[],h=[];"
      "for(let i=0;i<N;i++){t.push(i-N+1);r.push(null);w.push(null);"
      "n.push(null);a.push(null);c.push(null);h.push(null);}"
      "const u=new uPlot({width:900,height:320,"
      "scales:{x:{time:false},y:{},kb:{}},"
      "axes:[{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{stroke:'#aaa',grid:{stroke:'#333'}},"
      "{side:1,scale:'kb',stroke:'#9ca3af',grid:{show:false},"
      "values:(u,vs)=>vs.map(v=>v+' KB')}],"
      "series:[{label:'t (s ago)'},"
      "{label:'reads/s',stroke:'#4ade80',width:2},"
      "{label:'writes/s',stroke:'#60a5fa',width:2},"
      "{label:'notifies/s',stroke:'#f472b6',width:2},"
      "{label:'adverts/s',stroke:'#fbbf24',width:2},"
      "{label:'connections',stroke:'#a78bfa',width:2},"
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
      "let logSeq=0;const con=document.getElementById('console');"
      "const ANSI=/\\x1b\\[[0-9;]*m/g;"
      "async function pollLog(){"
      "try{const r=await fetch('/log?since='+logSeq);"
      "logSeq=parseInt(r.headers.get('X-Log-Seq')||logSeq,10);"
      "let txt=await r.text();"
      "if(txt){txt=txt.replace(ANSI,'');"
      "const atBottom=con.scrollHeight-con.scrollTop-con.clientHeight<20;"
      "con.textContent+=txt;"
      "if(con.textContent.length>50000)"
      "con.textContent=con.textContent.slice(-40000);"
      "if(atBottom)con.scrollTop=con.scrollHeight;"
      "}}catch(e){}}"
      "setInterval(pollLog,500);pollLog();"
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, sizeof(page) - 1);
}

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }

void install_log_hook() {
  if (g_log_mutex != nullptr) return;  // already installed
  g_log_mutex = xSemaphoreCreateMutex();
  g_old_vprintf = esp_log_set_vprintf(&log_vprintf);
  if (g_old_vprintf == nullptr) g_old_vprintf = &std::vprintf;
  ESP_LOGI(TAG, "log hook installed, %u-byte ring",
           static_cast<unsigned>(LOG_RING_SIZE));
}

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
  httpd_uri_t log = {.uri = "/log",
                     .method = HTTP_GET,
                     .handler = &log_get,
                     .user_ctx = nullptr};
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &stats);
  httpd_register_uri_handler(srv, &log);
  ESP_LOGI(TAG, "stats UI at /");
}

}  // namespace api_server::stats
