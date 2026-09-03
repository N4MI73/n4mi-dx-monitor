#pragma once
#include <Arduino.h>
#include "config.h"

/**
 * Mirrors the real /api/dxmon/watched shape, confirmed live 2026-08-29
 * (not assumed from the Joplin note's documentation alone -- the note only
 * described the per-entry shape, not whether the endpoint wraps it).
 *
 * last_spot can be genuinely null (an entry that's "active" per ADXO's date
 * window but hasn't actually been spotted yet) -- confirmed with real data,
 * not a hypothetical edge case.
 */
struct WatchedEntry {
    char callsign[16];
    char dxcc[48];
    bool adxo_active;
    char adxo_end[16];        // raw "YYYY-MM-DD"
    bool has_last_spot;
    char band[8];
    char mode[16];             // raw lowercase, e.g. "cw" -- uppercase at display time
    char frequency[16];        // raw string, e.g. "18.0847"
    char received_at[32];      // raw ISO 8601 -- see main.cpp's format_short_datetime()
    char comment[64];          // operator comment on the spot -- only present on real
                                // "cluster" source spots (a human typed it); empty on
                                // automated sources (rbn/pskreporter). Confirmed real
                                // 2026-09-01 via the /hamalert curation page.
};

struct WatchedData {
    WatchedEntry entries[MAX_WATCHED_ENTRIES];
    int count;
    char updated[32];   // raw ISO 8601, the server's own clock at fetch time -- used as the
                         // "now" reference for relative-day math (e.g. "Starts in 6 days") on
                         // the Watched roster screen, avoiding any need for device-side NTP/RTC.
};

/**
 * Mirrors /api/preview/status, confirmed live 2026-09-01 -- backend health
 * data the virtual web preview's own Config tab already uses. Note: no
 * HamAlert last-spot timestamp is present at this endpoint (unlike the
 * mockup's "last spot 2m ago" detail) -- deliberately not reconstructed from
 * watched-entry data to avoid reintroducing the elapsed-time math already
 * scoped out of firmware for this pass.
 */
struct PreviewStatus {
    int adxo_entry_count;
    char adxo_updated[32];     // raw ISO 8601
    bool hamalert_connected;
    bool hamalert_enabled;
    bool hamalert_logged_in;
    int watched_count;
};

bool dxmon_fetch_preview_status(PreviewStatus &out);

/**
 * Mirrors /api/dxmon/targets, confirmed live 2026-09-02 -- the merged
 * Needed+Wanted feed. Needed entries carry `prefix` (may be empty for a
 * couple of real entities, e.g. Spratly Islands); Wanted entries carry
 * `band`/`mode`/`note` instead. `band` is sized generously (24 chars) since
 * a real Wanted entry can hold a multi-value string like "17M, 15M", not
 * just a single band.
 *
 * `last_spot` = strictly "currently in HamAlert's live recent-spots buffer
 * right now" -- a real-time signal. `last_seen` = the persisted record of
 * the most recent real hit ever, which may equal last_spot (just hit) or be
 * older, surviving after last_spot ages out of that buffer. Both
 * independently nullable; confirmed both null together is the real,
 * expected state for an entity with no history at all.
 */
struct SpotInfo {
    bool present;
    char callsign[16];
    char band[8];
    char mode[16];
    char frequency[16];
    char received_at[32];
    char comment[64];
};

struct TargetEntry {
    char type[8];          // "needed" or "wanted"
    char entity[48];
    char prefix[16];       // needed only; may be empty string for a real entity
    char band[24];         // wanted only; may hold multiple values, e.g. "17M, 15M"
    char mode[16];         // wanted only; may be empty
    bool has_adxo;
    bool adxo_active;
    char adxo_begin[16];   // raw "YYYY-MM-DD"
    char adxo_end[16];     // raw "YYYY-MM-DD"
    SpotInfo last_spot;
    SpotInfo last_seen;
};

struct TargetsData {
    TargetEntry entries[MAX_TARGET_ENTRIES];
    int count;
    char updated[32];
};

bool dxmon_fetch_targets(TargetsData &out);

/**
 * Fetches and parses the current watched list. Parses into a local temporary
 * first and only commits to `out` on full success -- matches the series-wide
 * "never let a partial or malformed response corrupt existing good data"
 * convention (see PropMon's own data_client_fetch_live()).
 *
 * Returns false on any Wi-Fi, HTTP, or JSON-parse failure, leaving `out`
 * untouched.
 */
bool dxmon_fetch_watched(WatchedData &out);
