#include "stats.h"

#include "connection.h"
#include "esp_log.h"
#include "esp_system.h"
#include "proxy_config.h"
#include "scanner.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace api_server::stats {

namespace {

constexpr const char *TAG = "stats";

std::atomic<uint32_t> g_reads{0};
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_notifies{0};

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
      "h1{font-size:1.1em;margin:0 0 1em}"
      "#chart{background:#1a1a1a;padding:.5em;border-radius:6px;"
      "display:inline-block}"
      "footer{margin-top:1em;color:#888;font-size:.85em}"
      "code{color:#bbb}"
      "</style></head><body>"
      "<h1>nimble-ble-proxy &mdash; BLE activity/s</h1>"
      "<div id=chart></div>"
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
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, sizeof(page) - 1);
}

}  // namespace

void record_read() { g_reads.fetch_add(1, std::memory_order_relaxed); }
void record_write() { g_writes.fetch_add(1, std::memory_order_relaxed); }
void record_notify() { g_notifies.fetch_add(1, std::memory_order_relaxed); }

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
  httpd_register_uri_handler(srv, &root);
  httpd_register_uri_handler(srv, &stats);
  ESP_LOGI(TAG, "stats UI at /");
}

}  // namespace api_server::stats
