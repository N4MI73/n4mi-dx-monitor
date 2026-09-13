#pragma once
#include <Arduino.h>

// Real captive-portal Wi-Fi setup foundation (2026-09-13), matching the
// series' own proven pattern from PropMon (2026-07-16) and APRSMon
// (2026-08-02): NVS-backed credential storage with a one-time, transparent
// migration from the old hardcoded wifi_credentials.h values. This means
// Dan's existing hardcoded credentials get migrated into NVS the first time
// a board boots this code -- not lost, not requiring a manual step -- and
// the storage layer works correctly even before wifi_portal.h/.cpp exists
// to actually let someone change networks from a real form.

// Replaces the old wifi_connect(). Tries stored NVS credentials first; if
// none exist yet, falls back to the hardcoded WIFI_SSID/WIFI_PASSWORD from
// wifi_credentials.h and seeds NVS with them on success. Blocking, up to
// timeout_ms -- same behavior as the function it replaces, just with real
// persistent storage underneath.
bool wifi_client_connect(uint32_t timeout_ms);

// Real, clean test of one specific set of credentials -- used by the
// captive portal's validation step (wifi_portal.cpp), not by normal boot.
// Explicitly disconnects first: without that, a device already connected to
// ANY network can silently report "success" without ever trying the
// submitted credentials at all -- a real, confirmed bug in PropMon's first
// build (2026-07-17) that let a deliberately wrong password get accepted,
// and then get saved over the real one. Deliberately does NOT call
// WiFi.mode() -- the caller has already put the radio in AP_STA mode for
// the portal's own AP interface, and touching that here would risk
// dropping the phone's connection to the AP mid-flow. Blocking, up to
// WIFI_CONNECT_TIMEOUT_MS (config.h).
bool wifi_client_try_credentials(const char *ssid, const char *password);

// Persists credentials to NVS. Called by the portal only after
// wifi_client_try_credentials() has already confirmed they actually work.
void wifi_client_save_credentials(const char *ssid, const char *password);
