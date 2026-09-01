#pragma once

// DXMon server -- confirmed live 2026-08-29 via a direct hit against the real endpoint.
#define DXMON_SERVER_HOST   "192.168.6.29"
#define DXMON_SERVER_PORT   8083
#define DXMON_WATCHED_PATH  "/api/dxmon/watched"
#define DXMON_PREVIEW_STATUS_PATH  "/api/preview/status"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define LIVE_FETCH_INTERVAL_MS   60000

#define MAX_WATCHED_ENTRIES  10
