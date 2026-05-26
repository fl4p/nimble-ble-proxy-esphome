#include "publish.h"

namespace ble_backend::publish {

namespace {
SendFn g_send = nullptr;
HasClientFn g_has = nullptr;
}  // namespace

void install(SendFn send, HasClientFn has_client) {
  g_send = send;
  g_has = has_client;
}

bool send_async(uint16_t msg_type, EncodeFn encode, void *ctx) {
  if (g_send == nullptr) return false;
  return g_send(msg_type, encode, ctx);
}

bool has_client() {
  if (g_has == nullptr) return false;
  return g_has();
}

}  // namespace ble_backend::publish
