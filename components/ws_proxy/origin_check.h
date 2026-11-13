// WebSocket handshake Origin policy for the /api bridge. Pure functions
// with no IDF dependency, so scripts/test_ws_origin.sh can compile and
// test them on the host.
//
// Why: browsers apply no CORS to WebSockets. Without this check any page
// open on the LAN could open ws://<proxy>/api and drive the full native
// API (GATT writes, PAIR/UNPAIR). A browser always sends Origin on a
// WebSocket handshake and a page cannot forge it, so:
//
//   * Origin absent (or empty)        -> accept. Not a browser:
//                                        aioesphomeapi over ws, curl, the
//                                        test scripts. Anything that can
//                                        omit Origin can also open :6053.
//   * Origin "null"                   -> refuse (file://, sandboxed iframe,
//                                        opaque redirects).
//   * Origin scheme://host[:port] whose host[:port] equals the Host header
//                                     -> accept (the on-device dashboard).
//                                        A default port (:80 for http,
//                                        :443 for https) on either side is
//                                        ignored; comparison is case-
//                                        insensitive.
//   * Origin listed verbatim in the allowlist (CONFIG_NBP_WS_ORIGIN_ALLOWLIST,
//     space/comma separated, case-insensitive, no trailing slash)
//                                     -> accept.
//   * anything else, including a missing Host or an Origin with a path,
//     userinfo or a non-http(s) scheme -> refuse.
//
// Not covered: DNS rebinding. A page on evil.example whose name later
// resolves to the proxy sends Origin http://evil.example and Host
// evil.example, which is same-origin by this rule (and by the browser's).
// Closing that needs a Host allowlist on every route, not just /api.

#pragma once

#include <cstddef>
#include <cstring>

namespace ws_proxy {
namespace origin {

enum class Verdict {
  kAcceptNoOrigin,
  kAcceptSameOrigin,
  kAcceptAllowlist,
  kRejectNull,
  kRejectNoHost,
  kRejectMalformed,
  kRejectForeign,
};

inline bool accepted(Verdict v) {
  return v == Verdict::kAcceptNoOrigin || v == Verdict::kAcceptSameOrigin ||
         v == Verdict::kAcceptAllowlist;
}

inline const char *name(Verdict v) {
  switch (v) {
    case Verdict::kAcceptNoOrigin: return "no-origin";
    case Verdict::kAcceptSameOrigin: return "same-origin";
    case Verdict::kAcceptAllowlist: return "allowlist";
    case Verdict::kRejectNull: return "null-origin";
    case Verdict::kRejectNoHost: return "no-host";
    case Verdict::kRejectMalformed: return "malformed-origin";
    case Verdict::kRejectForeign: return "foreign-origin";
  }
  return "?";
}

namespace detail {

inline char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool ieq(const char *a, size_t an, const char *b, size_t bn) {
  if (an != bn) return false;
  for (size_t i = 0; i < an; ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

inline bool iprefix(const char *s, size_t n, const char *p) {
  size_t pn = std::strlen(p);
  return n >= pn && ieq(s, pn, p, pn);
}

// Drop a trailing ":<port>" when it equals the scheme's default port.
// Bracketed IPv6 literals keep their inner colons.
inline size_t strip_default_port(const char *s, size_t n, const char *port) {
  size_t pn = std::strlen(port);  // e.g. ":80"
  if (n > pn && ieq(s + n - pn, pn, port, pn)) return n - pn;
  return n;
}

inline bool is_space_or_comma(char c) {
  return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace detail

// True if `origin` (exact serialized origin) appears in `allowlist`.
inline bool in_allowlist(const char *origin, const char *allowlist) {
  if (origin == nullptr || allowlist == nullptr) return false;
  size_t on = std::strlen(origin);
  const char *p = allowlist;
  while (*p != '\0') {
    while (*p != '\0' && detail::is_space_or_comma(*p)) ++p;
    const char *start = p;
    while (*p != '\0' && !detail::is_space_or_comma(*p)) ++p;
    size_t len = static_cast<size_t>(p - start);
    if (len > 0 && detail::ieq(start, len, origin, on)) return true;
  }
  return false;
}

// `origin` / `host`: header values, or nullptr when the header is absent.
inline Verdict check(const char *origin, const char *host,
                     const char *allowlist) {
  using namespace detail;
  if (origin == nullptr || origin[0] == '\0') return Verdict::kAcceptNoOrigin;
  size_t on = std::strlen(origin);
  if (ieq(origin, on, "null", 4)) return Verdict::kRejectNull;
  if (in_allowlist(origin, allowlist)) return Verdict::kAcceptAllowlist;

  const char *default_port;
  size_t skip;
  if (iprefix(origin, on, "http://")) {
    skip = 7;
    default_port = ":80";
  } else if (iprefix(origin, on, "https://")) {
    skip = 8;
    default_port = ":443";
  } else {
    return Verdict::kRejectMalformed;
  }
  const char *oh = origin + skip;
  size_t ohn = on - skip;
  if (ohn == 0) return Verdict::kRejectMalformed;
  for (size_t i = 0; i < ohn; ++i) {
    char c = oh[i];
    // A serialized origin has no path, query, fragment, userinfo or
    // whitespace.
    if (c == '/' || c == '?' || c == '#' || c == '@' || c == ' ' ||
        c == '\t') {
      return Verdict::kRejectMalformed;
    }
  }

  if (host == nullptr || host[0] == '\0') return Verdict::kRejectNoHost;
  size_t hn = std::strlen(host);

  ohn = strip_default_port(oh, ohn, default_port);
  hn = strip_default_port(host, hn, default_port);
  if (ieq(oh, ohn, host, hn)) return Verdict::kAcceptSameOrigin;
  return Verdict::kRejectForeign;
}

}  // namespace origin
}  // namespace ws_proxy
