#pragma once

// DXMon server -- confirmed live 2026-08-29 via a direct hit against the real endpoint.
#define DXMON_SERVER_HOST   "192.168.6.29"
#define DXMON_SERVER_PORT   8083
#define DXMON_WATCHED_PATH  "/api/dxmon/watched"
#define DXMON_NEEDED_PATH   "/api/dxmon/needed"
#define DXMON_PREVIEW_STATUS_PATH  "/api/preview/status"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define LIVE_FETCH_INTERVAL_MS   60000

#define MAX_WATCHED_ENTRIES  10
// Unified Needed/Wanted redesign, 2026-09-04: /api/dxmon/needed now serves only
// Dan's manually curated list (needed.json), not a full no_confirms.csv sweep --
// no longer expected to run anywhere near the old 66-70-entry scale. 30 gives
// real headroom over the two entries live today while staying small. This is
// the first real value picked for the new shape, not yet load-tested against a
// larger curated list -- raise it if Dan's real list ever approaches it.
#define MAX_NEEDED_ENTRIES   30
