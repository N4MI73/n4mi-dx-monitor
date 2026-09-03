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
 * Mirrors /api/dxmon/needed (renamed from /api/dxmon/targets 2026-09-04, when
 * Needed and Wanted merged into one curated list -- see the series brief/Joplin
 * note for the full design). No more ADXO cross-reference and no more
 * no_confirms.csv "prefix" field -- both were specific to the old CSV-driven
 * Needed path, which no longer exists; ADXO stays Watched-only going forward.
 *
 * `kind` is a derived (not stored) server-side value: "entity" when both band
 * and mode are blank (a whole never-confirmed-entity target, the old Needed
 * semantic), "slot" when either is set (a specific band/mode gap on an
 * already-confirmed entity, the old Wanted semantic). `id` is the entry's own
 * stable id from needed.json -- kept so two entries sharing the same entity
 * (e.g. a whole-entity target and a separate specific-slot target on the same
 * DXCC) can still be told apart if a future screen needs to reference one
 * specifically (e.g. a row-tap detail view).
 *
 * `last_spot` = strictly "currently in HamAlert's live recent-spots buffer
 * right now" -- a real-time signal. `last_seen` = the persisted record of
 * the most recent real hit ever, which may equal last_spot (just hit) or be
 * older, surviving after last_spot ages out of that buffer. Both
 * independently nullable; confirmed both null together is the real,
 * expected state for a newly-added entry with no history yet.
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

struct NeededEntry {
    char id[16];            // stable id from needed.json, e.g. "a1b2c3d4e5f6"
    char kind[8];           // "entity" or "slot" -- derived server-side, see comment above
    char entity[48];
    char band[24];          // slot kind only; may hold multiple values, e.g. "17M, 15M"
    char mode[16];          // slot kind only; may be empty
    SpotInfo last_spot;
    SpotInfo last_seen;
};

struct NeededData {
    NeededEntry entries[MAX_NEEDED_ENTRIES];
    int count;
    char updated[32];
};

bool dxmon_fetch_needed(NeededData &out);

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
