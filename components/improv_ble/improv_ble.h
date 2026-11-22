// Improv Wi-Fi over BLE (https://www.improv-wifi.com/ble/) for the
// provisioning window: a phone gives the device WiFi credentials over BLE
// while it has no STA uplink.

#pragma once

#include "wifi_sta.h"

namespace improv_ble {

// Hooks for wifi_sta::set_provisioning_hooks(). `entered` brings up NimBLE
// with the Improv GATT service and advertising; nothing stops them, the
// device leaves the provisioning window only by rebooting.
const wifi_sta::ProvisioningHooks *hooks();

}  // namespace improv_ble
