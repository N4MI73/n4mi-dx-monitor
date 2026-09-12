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
    // Added 2026-09-12: PropMon's current condition for this spot's own band
    // ("good"/"fair"/"poor"), or empty string if unavailable (PropMon
    // unreachable, or the band wasn't in PropMon's response) -- same
    // null-as-empty-string convention already used for `comment` below.
    // Real motivation (Dan): DXMon and PropMon aren't guaranteed to be in
    // the same physical spot, so a spot's frequency alone doesn't say
    // whether that band is actually any good right now.
    char band_condition[8];
    char comment[64];          // operator comment on the spot -- only present on real
                                // "cluster" source spots (a human typed it); empty on
                                // automated sources (rbn/pskreporter). Confirmed real
                                // 2026-09-01 via the /hamalert curation page.
    // Added 2026-09-06: beam heading to this entry's own callsign -- already
    // sent by /api/dxmon/watched as a top-level "beam" field (not nested under
    // last_spot, since a Watched entry has one fixed callsign, unlike Needed
    // where the callsign varies per spot). The web /watched page has shown
    // this since 2026-08-25; this was purely a firmware display gap, same
    // pattern as the Needed callsign gap found and fixed 2026-09-05.
    bool has_beam;
    float heading_deg;
    int distance_mi;
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
    // Added 2026-09-12: same PropMon band-condition field as WatchedEntry's
    // own -- see its comment there for the full design. Applies to both
    // last_spot and last_seen (this struct is shared by both), even though
    // firmware currently only ever displays it for a live (last_spot) hit --
    // Overview's Tier 2 doesn't show frequency/band at all today, so there's
    // no natural place to show a band condition there yet either.
    char band_condition[8];
    // Added 2026-09-05: beam heading to the spotted callsign, for Needed hits only
    // (Watched already shows its own callsign prominently as the entry itself, so
    // this wasn't requested there). Mirrors /api/dxmon/needed's own "beam" field,
    // computed server-side fresh per request -- never persisted, since a heading
    // to a fixed callsign doesn't change over time.
    bool has_beam;
    float heading_deg;
    int distance_mi;
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
 * Category Activity Feed (2026-09-06) -- one flat list of recent real spots
 * across a whole category (Watched or Needed), fed by /api/dxmon/activity/
 * watched or .../needed. Deliberately NOT backed by the same persistent
 * spot_history.json the Single-Target History screen uses -- a category-wide
 * feed is broad enough that HamAlert's own live buffer is sufficient; the
 * aging-out problem spot_history.json solves is specific to narrow,
 * single-target queries. entity/kind are only populated for the Needed feed
 * (empty for Watched, where every entry already is one known callsign).
 */
struct ActivitySpot {
    char entity[48];      // empty for the Watched feed
    char kind[8];          // "entity" or "slot" -- empty for the Watched feed
    char callsign[16];
    char band[8];
    char mode[16];
    char frequency[16];
    char received_at[32];
    bool has_beam;
    float heading_deg;
    int distance_mi;
};

struct ActivityData {
    ActivitySpot spots[MAX_ACTIVITY_SPOTS];
    int count;
    char updated[32];
};

/**
 * Fetches either the Watched or Needed activity feed depending on
 * is_needed. Same temp-then-commit-on-success pattern as the other fetch
 * functions in this file.
 */
bool dxmon_fetch_activity(bool is_needed, ActivityData &out);

/**
 * Single-Target Spot History (2026-09-08) -- up to the last 10 real spots
 * for one specific target, fed by /api/dxmon/history/callsign/<callsign> or
 * .../needed/<needed_id>. Unlike the Category Activity Feed, this IS backed
 * by the persistent spot_history.json store -- a narrow, single-target
 * query is exactly the case HamAlert's shared ~100-spot live buffer can't
 * reliably answer (a rarer hit could easily have aged out).
 *
 * Two variants share this one struct, populated according to which fetch
 * function was called:
 * - Callsign-level (dxmon_fetch_history_callsign): opened from an
 *   individual-spot tap (Category Activity Feed row), or a Watched
 *   roster-row tap (a Watched entry already is one fixed callsign). `callsign`
 *   and the top-level `has_beam`/`heading_deg`/`distance_mi` are populated;
 *   `entity`/`band`/`mode` are empty. Every spot shares the same callsign, so
 *   beam heading is only computed once, at the top level.
 * - Entity-level (dxmon_fetch_history_needed): opened from a Needed
 *   roster-row tap. `entity`/`band`/`mode` are populated; `callsign` and the
 *   top-level beam fields are empty. An entity/slot can be worked by several
 *   different callsigns over time (confirmed live -- Singapore has both
 *   9V1XX and 9V1SH real spots), so each HistorySpot carries its OWN beam
 *   heading rather than sharing one top-level value.
 */
struct HistorySpot {
    char callsign[16];
    char band[8];
    char mode[16];
    char frequency[16];
    char received_at[32];
    bool has_beam;
    float heading_deg;
    int distance_mi;
};

struct HistoryData {
    HistorySpot spots[MAX_HISTORY_SPOTS];
    int count;
    char updated[32];

    // Header context -- exactly one of these two groups is populated,
    // depending on which fetch function was called (see struct comment above).
    char callsign[16];    // callsign-level only
    char entity[48];      // entity-level only
    char band[24];        // entity-level only -- may hold multiple values, e.g. "17M, 15M"
    char mode[16];        // entity-level only

    bool has_beam;         // top-level beam -- callsign-level only
    float heading_deg;
    int distance_mi;
};

/**
 * Fetches the callsign-level Single-Target Spot History. `callsign` is
 * URL-appended to DXMON_HISTORY_CALLSIGN_PATH -- caller is responsible for
 * passing a value safe to appear directly in a URL path segment (a real
 * callsign, e.g. "5A1AL", always is).
 */
bool dxmon_fetch_history_callsign(const char *callsign, HistoryData &out);

/**
 * Fetches the entity-level Single-Target Spot History. `needed_id` is
 * URL-appended to DXMON_HISTORY_NEEDED_PATH -- this is always a Needed
 * entry's own stable id (e.g. "8ee29de95330"), never user-entered text.
 */
bool dxmon_fetch_history_needed(const char *needed_id, HistoryData &out);

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
