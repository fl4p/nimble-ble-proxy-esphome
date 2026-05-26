// Advertise this device on the LAN as a regular ESPHome native-API
// node so Home Assistant's ESPHome integration auto-discovers it.
//
// Service: _esphomelib._tcp on proxy::API_PORT.
// TXT records mirror esphome/components/mdns/mdns_component.cpp:78-182.

#pragma once

namespace mdns_announce {

void start();

}  // namespace mdns_announce
