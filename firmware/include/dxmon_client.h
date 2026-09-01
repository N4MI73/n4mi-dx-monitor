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
 * Fetches and parses the current watched list. Parses into a local temporary
 * first and only commits to `out` on full success -- matches the series-wide
 * "never let a partial or malformed response corrupt existing good data"
 * convention (see PropMon's own data_client_fetch_live()).
 *
 * Returns false on any Wi-Fi, HTTP, or JSON-parse failure, leaving `out`
 * untouched.
 */
bool dxmon_fetch_watched(WatchedData &out);
