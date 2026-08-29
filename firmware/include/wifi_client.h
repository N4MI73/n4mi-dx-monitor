#pragma once
#include <Arduino.h>

// Blocking connect, matching the same proof-of-concept sequencing PropMon
// and APRSMon both used before their real captive portals existed.
bool wifi_connect(uint32_t timeout_ms);
