// Host test for improv_proto (RPC assembly, Wi-Fi settings parsing, result
// encoding, service data).
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I components/improv_ble \
//       components/improv_ble/improv_proto.cpp \
//       components/improv_ble/host_test/test_improv_proto.cpp -o /tmp/t && /tmp/t
//
// `/tmp/t --pipe` reads commands on stdin for scripts/test_improv_client.py,
// which checks the encoding against py-improv-ble-client:
//   F <hex> [ms]        feed one write  -> PKT <hex> [WIFI <ssid> <psk>] | MORE | ERR
//   R <cmd> <cap> <hex>...  build a result -> RES <hex> <strings added>
//   S <state> <caps>    service data    -> SD <hex>

#include "improv_proto.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace improv;

static int g_fail = 0;

static void check(bool ok, const char *what) {
  if (!ok) ++g_fail;
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    v.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
  }
  return v;
}

static std::string hex(const uint8_t *p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
  }
  return s;
}

static std::vector<uint8_t> packet(uint8_t cmd, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> p{cmd, static_cast<uint8_t>(data.size())};
  p.insert(p.end(), data.begin(), data.end());
  p.push_back(checksum(p.data(), p.size()));
  return p;
}

static std::vector<uint8_t> wifi_packet(const std::string &ssid, const std::string &psk) {
  std::vector<uint8_t> d{static_cast<uint8_t>(ssid.size())};
  d.insert(d.end(), ssid.begin(), ssid.end());
  d.push_back(static_cast<uint8_t>(psk.size()));
  d.insert(d.end(), psk.begin(), psk.end());
  return packet(CMD_WIFI_SETTINGS, d);
}

static int feed(Assembler &a, const std::vector<uint8_t> &c, uint32_t ms, uint8_t *out) {
  return a.feed(c.data(), c.size(), ms, out);
}

static int pipe_mode() {
  Assembler a;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string op;
    in >> op;
    if (op == "F") {
      std::string h;
      uint32_t ms = 0;
      in >> h >> ms;
      const auto c = unhex(h);
      uint8_t out[MAX_PACKET];
      const int n = a.feed(c.data(), c.size(), ms, out);
      if (n < 0) {
        std::cout << "ERR\n";
      } else if (n == 0) {
        std::cout << "MORE\n";
      } else {
        std::cout << "PKT " << hex(out, n);
        if (out[0] == CMD_WIFI_SETTINGS) {
          const uint8_t *s, *p;
          size_t sl, pl;
          const uint8_t e = parse_wifi_settings(out + 2, out[1], &s, &sl, &p, &pl);
          if (e == ERR_NONE) {
            std::cout << " WIFI " << (sl ? hex(s, sl) : "-") << " " << (pl ? hex(p, pl) : "-");
          } else {
            std::cout << " WIFIERR " << int(e);
          }
        }
        std::cout << "\n";
      }
    } else if (op == "R") {
      int cmd = 0;
      size_t cap = 0;
      in >> cmd >> cap;
      uint8_t buf[MAX_PACKET];
      Result r(static_cast<uint8_t>(cmd), buf, cap);
      std::string h;
      int added = 0;
      while (in >> h) {
        const auto s = h == "-" ? std::vector<uint8_t>{} : unhex(h);
        if (!r.add(reinterpret_cast<const char *>(s.data()), s.size())) break;
        ++added;
      }
      const size_t n = r.finish();
      std::cout << "RES " << hex(buf, n) << " " << added << "\n";
    } else if (op == "S") {
      int st = 0, caps = 0;
      in >> st >> caps;
      uint8_t sd[6];
      service_data(static_cast<uint8_t>(st), static_cast<uint8_t>(caps), sd);
      std::cout << "SD " << hex(sd, 6) << "\n";
    }
    std::cout.flush();
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && std::strcmp(argv[1], "--pipe") == 0) return pipe_mode();

  uint8_t out[MAX_PACKET];

  // Spec example: SSID MyWirelessAP, password mysecurepassword -> 01 1E 0C ...
  const auto ex = wifi_packet("MyWirelessAP", "mysecurepassword");
  check(ex[0] == 0x01 && ex[1] == 0x1E && ex[2] == 0x0C, "spec example header 01 1E 0C");
  {
    Assembler a;
    check(feed(a, ex, 0, out) == static_cast<int>(ex.size()), "whole packet in one write");
    const uint8_t *s, *p;
    size_t sl, pl;
    check(parse_wifi_settings(out + 2, out[1], &s, &sl, &p, &pl) == ERR_NONE &&
              sl == 12 && std::memcmp(s, "MyWirelessAP", 12) == 0 && pl == 16 &&
              std::memcmp(p, "mysecurepassword", 16) == 0,
          "spec example parses to SSID/password");
  }
  {
    Assembler a;  // 20-byte writes, as a client on a 23-byte MTU sends them
    std::vector<uint8_t> c1(ex.begin(), ex.begin() + 20), c2(ex.begin() + 20, ex.end());
    check(feed(a, c1, 0, out) == 0 && feed(a, c2, 10, out) == static_cast<int>(ex.size()),
          "packet split over two writes");
  }
  {
    Assembler a;
    auto bad = ex;
    bad.back() ^= 1;
    check(feed(a, bad, 0, out) < 0, "bad checksum refused");
    check(feed(a, ex, 1, out) == static_cast<int>(ex.size()), "next packet after a bad one");
  }
  {
    Assembler a;
    auto longer = ex;
    longer.push_back(0);
    check(feed(a, longer, 0, out) < 0, "bytes beyond the packet length refused");
  }
  {
    // Review 2026-09-17 finding 6: the continuation of a split packet is a
    // checksum-valid packet itself. It must complete the outer packet.
    const auto c1 = unhex("0116076f75746572517e0d70616464696e67");
    const auto c2 = unhex("0104024142008a");
    Assembler a;
    check(feed(a, c1, 0, out) == 0, "adversarial split: first part waits");
    const int n = feed(a, c2, 1, out);
    const uint8_t *s, *p;
    size_t sl, pl;
    check(n == 25 && parse_wifi_settings(out + 2, out[1], &s, &sl, &p, &pl) == ERR_NONE &&
              sl == 7 && std::memcmp(s, "outerQ~", 7) == 0 && pl == 13,
          "adversarial split: continuation completes the outer packet");
  }
  {
    // A stale partial packet is dropped; the next write starts afresh. The
    // second write is NOT a whole packet, so only the timeout can drop it.
    Assembler a;
    std::vector<uint8_t> c1(ex.begin(), ex.begin() + 20);
    std::vector<uint8_t> head(ex.begin(), ex.begin() + 20);
    std::vector<uint8_t> tail(ex.begin() + 20, ex.end());
    check(feed(a, c1, 0, out) == 0, "stale: partial held");
    check(feed(a, head, ASSEMBLY_TIMEOUT_MS + 1, out) == 0 &&
              feed(a, tail, ASSEMBLY_TIMEOUT_MS + 2, out) == static_cast<int>(ex.size()),
          "stale: partial dropped after the timeout, new packet assembles");
    Assembler b;
    check(feed(b, c1, 0, out) == 0 && feed(b, head, ASSEMBLY_TIMEOUT_MS - 1, out) < 0,
          "fresh: the same writes inside the timeout make a malformed packet");
  }
  {
    // A whole new packet replaces an unrelated partial one.
    Assembler a;
    std::vector<uint8_t> c1(ex.begin(), ex.begin() + 20);
    const auto info = packet(CMD_DEVICE_INFO, {});
    check(feed(a, c1, 0, out) == 0 && feed(a, info, 1, out) == 3 && out[0] == CMD_DEVICE_INFO,
          "whole packet replaces a partial one");
  }
  {
    Assembler a;
    std::vector<uint8_t> huge(MAX_PACKET + 1, 0x01);
    check(feed(a, huge, 0, out) < 0, "overlong write refused");
    check(feed(a, {0x03}, 1, out) == 0 && feed(a, {0x00, 0x03}, 2, out) == 3,
          "byte-wise writes assemble");
  }

  const uint8_t *s, *p;
  size_t sl, pl;
  const uint8_t open_net[] = {3, 'a', 'b', 'c', 0};
  check(parse_wifi_settings(open_net, 5, &s, &sl, &p, &pl) == ERR_NONE && pl == 0,
        "empty password (open network)");
  const uint8_t empty_ssid[] = {0, 0};
  check(parse_wifi_settings(empty_ssid, 2, &s, &sl, &p, &pl) == ERR_INVALID_RPC,
        "empty SSID refused");
  const uint8_t short_d[] = {3, 'a', 'b', 'c'};
  check(parse_wifi_settings(short_d, 4, &s, &sl, &p, &pl) == ERR_INVALID_RPC,
        "missing password length refused");
  const uint8_t over[] = {1, 'a', 3, 'x', 'y'};
  check(parse_wifi_settings(over, 5, &s, &sl, &p, &pl) == ERR_INVALID_RPC,
        "password length past the end refused");
  const uint8_t extra[] = {1, 'a', 1, 'x', 'y'};
  check(parse_wifi_settings(extra, 5, &s, &sl, &p, &pl) == ERR_INVALID_RPC,
        "trailing bytes refused");

  {
    uint8_t buf[MAX_PACKET];
    Result r(CMD_WIFI_SETTINGS, buf, sizeof(buf));
    check(r.add("http://192.168.1.5/"), "result: add URL");
    const size_t n = r.finish();
    check(n == 2 + 1 + 19 + 1 && buf[0] == 1 && buf[1] == 20 && buf[2] == 19 &&
              buf[n - 1] == checksum(buf, n - 1),
          "result: layout and checksum");
    Result e(CMD_WIFI_SETTINGS, buf, sizeof(buf));
    check(e.finish() == 3 && buf[1] == 0 && buf[2] == 1, "result: empty list");
    Result small(CMD_DEVICE_INFO, buf, 20);
    check(small.add("nimble-ble") && !small.add("x123456"), "result: refuses past cap");
    check(small.finish() == 14, "result: cap leaves the packet whole");
    Result big(CMD_SCAN, buf, sizeof(buf));
    std::string s100(100, 'a');
    check(big.add(s100.c_str()) && big.add(s100.c_str()) && !big.add(s100.c_str()),
          "result: data field capped at 255 bytes");
    const size_t m = big.mark();
    big.add("x");
    big.rewind(m);
    check(big.finish() == 2 + 202 + 1, "result: rewind drops a partial triple");
  }
  {
    uint8_t sd[6];
    service_data(STATE_AUTHORIZED, CAP_DEVICE_INFO | CAP_SCAN, sd);
    const uint8_t want[] = {0x02, 0x06, 0, 0, 0, 0};
    check(std::memcmp(sd, want, 6) == 0, "service data 02 06 00 00 00 00");
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
