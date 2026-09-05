#pragma once

// DXMon server -- confirmed live 2026-08-29 via a direct hit against the real endpoint.
#define DXMON_SERVER_HOST   "192.168.6.29"
#define DXMON_SERVER_PORT   8083
#define DXMON_WATCHED_PATH  "/api/dxmon/watched"
#define DXMON_NEEDED_PATH   "/api/dxmon/needed"
#define DXMON_PREVIEW_STATUS_PATH  "/api/preview/status"

#define WIFI_CONNECT_TIMEOUT_MS  15000
#define LIVE_FETCH_INTERVAL_MS   60000
// Mitigation for the still-unresolved display-rendering glitch (2026-09-04) -- root
// cause not found despite ruling out available heap and pixel-clock bandwidth as
// factors, and the display driver library doesn't expose the underrun/vsync callback
// hooks that would let us catch it directly (confirmed by inspecting BusRGB's real
// header). Real recurrence rate observed by Dan is well under an hour, not once-daily,
// so this uses a plain uptime interval (millis()-based) rather than a scheduled
// wall-clock time -- it needs to fire regardless of time of day. Real, honest cost:
// a brief blank/reconnecting screen every interval, even if someone's looking right
// at that moment -- accepted as the lesser inconvenience versus the glitch itself.
// Single constant, easy to retune if 30 min turns out too aggressive or not enough.
// Interval tuning history: 30 min (2026-09-04) -> 25 min (2026-09-05 morning, after a
// 21-minute onset was observed) -> 15 min (2026-09-05 afternoon, after a real, clearly
// visible slip appeared within 22 minutes even at the 25-minute setting -- confirmed via
// photo: reboot at 13:43, substantial creep visible by 14:05, next scheduled reboot at
// 14:08 cleared it). 25 minutes still isn't reliably ahead of the glitch's real onset
// timing; tightening further to keep the visible window as small as possible.
#define SCHEDULED_REBOOT_INTERVAL_MS  (15UL * 60UL * 1000UL)

#define MAX_WATCHED_ENTRIES  10
// Unified Needed/Wanted redesign, 2026-09-04: /api/dxmon/needed now serves only
// Dan's manually curated list (needed.json), not a full no_confirms.csv sweep --
// no longer expected to run anywhere near the old 66-70-entry scale. 30 gives
// real headroom over the two entries live today while staying small. This is
// the first real value picked for the new shape, not yet load-tested against a
// larger curated list -- raise it if Dan's real list ever approaches it.
#define MAX_NEEDED_ENTRIES   30
