/**
 * N4MI DXMon -- real firmware UI with live data + real Config screen.
 *
 * Build order:
 *   1-3. Four-tab shell, live Overview data fetch -- confirmed 2026-08-29.
 *   4. This step (2026-09-01): real Config screen (Wi-Fi/ADXO/HamAlert
 *      status, watched count, curate-at URL, firmware version, Force
 *      Refresh), plus a small Wi-Fi glyph on Overview.
 *
 * Watched/Needed tabs remain honest "Not yet built" placeholders.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>  // added 2026-09-08 for the spot-staleness helper (mktime/struct tm/difftime)
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>

#include <lvgl.h>
#include "lvgl_v8_port.h"

#include "config.h"
#include "wifi_client.h"
#include "dxmon_client.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// ---------------------------------------------------------------------------
// Palette -- lifted directly from the approved mockups, not guessed.
// ---------------------------------------------------------------------------
#define COLOR_BG            lv_color_hex(0x0a0e14)
#define COLOR_HEADER_BG     lv_color_hex(0x12161f)
#define COLOR_DIVIDER       lv_color_hex(0x232935)
#define COLOR_PANEL_BG      lv_color_hex(0x111726)
#define COLOR_PANEL_BORDER  lv_color_hex(0x1d2536)
#define COLOR_TEXT_PRIMARY  lv_color_hex(0xe8ecf1)
#define COLOR_TEXT_SECOND   lv_color_hex(0xa8b2c4)
#define COLOR_TEXT_MUTED    lv_color_hex(0x6b7385)
#define COLOR_ACCENT_BLUE   lv_color_hex(0x4a9eff)
#define COLOR_BADGE_BLUE_BG lv_color_hex(0x1a2a44)
#define COLOR_BADGE_BLUE_TX lv_color_hex(0x7ab8ff)
#define COLOR_ACCENT_AMBER  lv_color_hex(0xffb84a)
#define COLOR_STATUS_GREEN  lv_color_hex(0x3ddc97)
#define COLOR_DOT_GRAY      lv_color_hex(0x5a6478)
#define COLOR_BADGE_BG      lv_color_hex(0x1a2233)
#define COLOR_BADGE_TEXT    lv_color_hex(0x8a94a6)
#define COLOR_STATUS_RED    lv_color_hex(0xe05c5c)  // 2026-09-12: PropMon "poor" band condition

// ---------------------------------------------------------------------------
// Lightweight date/time formatting -- see 2026-08-29 session notes: real
// elapsed-time math (NTP sync + full ISO parsing + day-count arithmetic) is
// deliberately out of scope. These just reformat raw ISO strings.
// ---------------------------------------------------------------------------
static const char *MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void format_short_date(const char *iso, char *out, size_t out_size)
{
    if (!iso || strlen(iso) < 10) {
        snprintf(out, out_size, "--");
        return;
    }
    int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month < 1 || month > 12) {
        snprintf(out, out_size, "--");
        return;
    }
    snprintf(out, out_size, "%s %d", MONTH_ABBR[month - 1], day);
}

static void format_short_datetime(const char *iso, char *out, size_t out_size)
{
    if (!iso || strlen(iso) < 16) {
        snprintf(out, out_size, "--");
        return;
    }
    int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month < 1 || month > 12) {
        snprintf(out, out_size, "--");
        return;
    }
    char hh[3] = {iso[11], iso[12], '\0'};
    char mm[3] = {iso[14], iso[15], '\0'};
    snprintf(out, out_size, "%s %d, %s:%s", MONTH_ABBR[month - 1], day, hh, mm);
}

static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = toupper((unsigned char)*s);
}

// Added 2026-09-08 for spot staleness visual treatment -- Dan's own real
// operational insight: a spot much older than a few hours has almost
// certainly moved on (different band/mode, or gone quiet), so an old spot
// shouldn't visually compete with a fresh one. 4 hours, Dan's own figure
// (given some leeway from an initial ~3-hour estimate). Named/scoped the
// same way as PropMon's own STALE_DATA_THRESHOLD_MS for the identical idea
// applied to a different kind of data.
#define STALE_SPOT_THRESHOLD_SEC (4UL * 60UL * 60UL)

/** Parses an ISO 8601 timestamp's date/time fields into epoch seconds via
 * mktime(). Deliberately ignores the timezone offset suffix (e.g. "-04:00")
 * entirely -- mktime() interprets the parsed fields as LOCAL time per the
 * device's own C library timezone setting, which won't generally match the
 * server's Eastern-time offset. This is fine and doesn't need fixing: every
 * staleness check here only ever computes the DIFFERENCE between two
 * timestamps parsed this exact same way (a spot's own received_at against
 * the response's own "updated" field), so any constant bias from mktime()'s
 * timezone handling cancels out in the subtraction -- the device never
 * needs a correctly configured timezone, or even a real-time clock synced
 * to now, for this to work correctly. Returns 0 on a malformed/too-short
 * string, treated by the caller as "can't tell, don't flag as stale." */
static time_t parse_iso8601_to_epoch(const char *iso)
{
    if (!iso || strlen(iso) < 19) return 0;
    struct tm tm_val = {0};
    tm_val.tm_year = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 +
                      (iso[2] - '0') * 10 + (iso[3] - '0') - 1900;
    tm_val.tm_mon  = (iso[5] - '0') * 10 + (iso[6] - '0') - 1;
    tm_val.tm_mday = (iso[8] - '0') * 10 + (iso[9] - '0');
    tm_val.tm_hour = (iso[11] - '0') * 10 + (iso[12] - '0');
    tm_val.tm_min  = (iso[14] - '0') * 10 + (iso[15] - '0');
    tm_val.tm_sec  = (iso[17] - '0') * 10 + (iso[18] - '0');
    return mktime(&tm_val);
}

/** True if spot_received_at is more than STALE_SPOT_THRESHOLD_SEC older than
 * response_updated_at (the screen's own "now" reference, from the same
 * fetch). Returns false (never flags stale) if either timestamp fails to
 * parse -- an unknown age is not evidence of staleness. */
static bool spot_is_stale(const char *spot_received_at, const char *response_updated_at)
{
    time_t spot_time = parse_iso8601_to_epoch(spot_received_at);
    time_t now_time = parse_iso8601_to_epoch(response_updated_at);
    if (spot_time == 0 || now_time == 0) return false;
    double elapsed = difftime(now_time, spot_time);
    return elapsed > (double)STALE_SPOT_THRESHOLD_SEC;
}

/** Picks fresh_color normally, or COLOR_TEXT_MUTED when is_stale -- a small
 * helper so staleness-aware coloring reads as a one-line substitution at
 * each call site rather than an if/else block repeated everywhere. */
static lv_color_t stale_aware_color(bool is_stale, lv_color_t fresh_color)
{
    return is_stale ? COLOR_TEXT_MUTED : fresh_color;
}

/** Maps a band_condition string ("good"/"fair"/"poor", or empty when PropMon
 * is unreachable or hasn't rated that band) to a dot color. 2026-09-12 --
 * reuses the same green/amber/red vocabulary already established elsewhere
 * (status dots, staleness) rather than inventing a new one. Unknown/empty
 * maps to the existing neutral gray dot color, matching how every other
 * "no data available" state in this app is already shown. */
static lv_color_t band_condition_color(const char *cond)
{
    if (!cond) return COLOR_DOT_GRAY;
    if (strcmp(cond, "good") == 0) return COLOR_STATUS_GREEN;
    if (strcmp(cond, "fair") == 0) return COLOR_ACCENT_AMBER;
    if (strcmp(cond, "poor") == 0) return COLOR_STATUS_RED;
    return COLOR_DOT_GRAY;
}

/** Shows/hides and positions a band-condition dot immediately to the right
 * of an already-laid-out label (typically a frequency label). Hides the dot
 * entirely rather than showing gray when cond is empty, on the theory that
 * "we don't know" is better shown as absence than as a fourth, easily-
 * confused-with-real-data color -- open to revisiting once this is actually
 * seen on real hardware. */
static void update_band_dot(lv_obj_t *dot, lv_obj_t *anchor_label, const char *cond)
{
    if (!cond || cond[0] == '\0') {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(dot, band_condition_color(cond), 0);
    lv_obj_update_layout(anchor_label);
    lv_obj_align_to(dot, anchor_label, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

/** Formats just "N deg / N mi" (empty string if no beam data) -- added
 * 2026-09-06 so the callsign and its beam heading can be shown as two
 * separately-styled labels (callsign made bold/bright/larger per Dan's
 * request) rather than one combined string. Deliberately uses the plain
 * ASCII "deg" abbreviation rather than a degree symbol -- the embedded
 * Montserrat bitmap font's Unicode coverage has already bitten this project
 * once (curly quotes rendering as tofu boxes on the WATCHED panel's comment
 * display, 2026-09-01); no reason to risk the same class of bug on an
 * untested glyph when a plain-ASCII alternative works.
 * Two overloads share this logic -- SpotInfo (Needed) and WatchedEntry
 * (Watched, added 2026-09-06) both carry has_beam/heading_deg/distance_mi
 * fields with the same names, but they're unrelated structs, so C++ won't
 * implicitly convert one to the other; a shared raw-values helper avoids
 * duplicating the actual formatting logic.
 * Distance in statute miles (2026-09-08, Dan's own preference -- U.S.
 * standard) -- the field itself and every JSON payload it's parsed from
 * were both renamed distance_km -> distance_mi at the same time, so this
 * is the server's own already-converted value, not a client-side
 * conversion happening here. */
static void format_beam_suffix_raw(bool has_beam, float heading_deg, int distance_mi,
                                    char *out, size_t out_size)
{
    if (has_beam) {
        snprintf(out, out_size, "%.0f deg / %d mi", heading_deg, distance_mi);
    } else {
        out[0] = '\0';
    }
}

static void format_beam_suffix(const SpotInfo &spot, char *out, size_t out_size)
{
    format_beam_suffix_raw(spot.has_beam, spot.heading_deg, spot.distance_mi, out, out_size);
}

static void format_beam_suffix(const WatchedEntry &e, char *out, size_t out_size)
{
    format_beam_suffix_raw(e.has_beam, e.heading_deg, e.distance_mi, out, out_size);
}

// ---------------------------------------------------------------------------
// Shared header -- title (left) + status text (right), matches every mockup.
// Returns the status label so callers needing extra header content (the
// Overview Wi-Fi glyph) can position relative to it robustly.
// ---------------------------------------------------------------------------
static lv_obj_t *create_header(lv_obj_t *parent, const char *title, const char *status)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, 56);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    lv_obj_t *title_lbl = lv_label_create(bar);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *status_lbl = lv_label_create(bar);
    lv_label_set_text(status_lbl, status);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -24, 0);

    lv_obj_t *divider = lv_obj_create(parent);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 800, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(divider, COLOR_DIVIDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    return status_lbl;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

// Forward declaration -- full implementation lives with the rest of the
// Category Activity Feed screen, after make_row_card() (which it reuses).
// Needed here so the Overview panels' tap handlers, built below, can call it.
static void open_activity_feed(bool is_needed);
static void watched_panel_click_cb(lv_event_t *e) { open_activity_feed(false); }
static void needed_panel_click_cb(lv_event_t *e) { open_activity_feed(true); }

// Forward declarations for the Single-Target Spot History screen (2026-09-08)
// -- full implementation lives after open_activity_feed(), but
// make_activity_row() (which appears before that) needs to call the
// callsign-level opener for its own "tap an individual spot" behavior.
static void open_history_callsign(const char *callsign);
static void open_history_needed(const char *needed_id);

// ---------------------------------------------------------------------------
// Small building blocks, shared across panels.
// ---------------------------------------------------------------------------
static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, COLOR_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    return panel;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                             lv_color_t color, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static lv_obj_t *make_divider(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, w, 1);
    lv_obj_set_pos(div, x, y);
    lv_obj_set_style_bg_color(div, COLOR_PANEL_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    return div;
}

/**
 * Right-aligned dot + label, robust to text-length changes -- fixes a real
 * clipping bug found on real hardware 2026-08-29 (see session notes).
 */
static lv_obj_t *make_status_indicator(lv_obj_t *parent, lv_obj_t **out_dot, lv_color_t dot_color,
                                        const char *text, lv_color_t text_color, int right_inset, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -right_inset, y);

    lv_obj_update_layout(lbl);
    int label_x = lv_obj_get_x(lbl);

    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, label_x - 10 - 13, y + 3);

    if (out_dot) *out_dot = dot;
    return lbl;
}

static void reposition_status_indicator(lv_obj_t *lbl, lv_obj_t *dot)
{
    lv_obj_update_layout(lbl);
    int label_x = lv_obj_get_x(lbl);
    int label_y = lv_obj_get_y(lbl);
    lv_obj_set_pos(dot, label_x - 10 - 13, label_y + 3);
}

static lv_obj_t *make_pill_badge(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 150, 26);
    lv_obj_set_pos(badge, x, y);
    lv_obj_set_style_bg_color(badge, COLOR_BADGE_BG, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 13, 0);
    lv_obj_t *lbl = lv_label_create(badge);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_center(lbl);
    return badge;
}

// ---------------------------------------------------------------------------
// Overview -- WATCHED panel is real and live; NEEDED panel is an honest
// placeholder (no backend exists for it yet). A small Wi-Fi glyph sits in
// the header, left of the "LIVE" status text.
// ---------------------------------------------------------------------------
struct OverviewWidgets {
    lv_obj_t *wifi_glyph;
    lv_obj_t *status_text;   // header's own status label, needed to position the glyph
    lv_obj_t *watched_panel; // 2026-09-12: whole-panel container, for the new-spot border flash
    lv_obj_t *watched_status_lbl;
    lv_obj_t *watched_status_dot;
    lv_obj_t *watched_callsign;
    lv_obj_t *watched_beam;   // added 2026-09-06 -- see make_screen_overview() for detail
    lv_obj_t *watched_dxcc;
    lv_obj_t *watched_freq;
    lv_obj_t *watched_band_dot;  // 2026-09-12: PropMon band-condition indicator
    lv_obj_t *watched_mode_badge;
    lv_obj_t *watched_mode_lbl;
    lv_obj_t *watched_last_spot;
    lv_obj_t *watched_comment;
    lv_obj_t *watched_active_through;
    lv_obj_t *watched_badge;
    lv_obj_t *watched_badge_lbl;
};
static OverviewWidgets ov;

/**
 * NEEDED panel widgets -- three tiers, all built up front, toggled via
 * LV_OBJ_FLAG_HIDDEN on each tier's own group container. See make_screen_overview()
 * for the full layout and the 2026-09-01/2026-09-02 design/backend history.
 * The kind badge (t1_type_*) shows ENTITY/SLOT (derived from band/mode presence,
 * 2026-09-04 unified-list redesign) -- reuses the same amber/blue identity colors
 * the old NEEDED/WANTED badge used, just renamed to match the new terminology.
 */
struct NeededWidgets {
    lv_obj_t *panel;  // 2026-09-12: whole-panel container, for the new-spot border flash
    lv_obj_t *status_lbl;
    lv_obj_t *loading_lbl;

    lv_obj_t *t1_group;
    lv_obj_t *t1_type_badge;
    lv_obj_t *t1_type_lbl;
    lv_obj_t *t1_entity;
    lv_obj_t *t1_subtitle;
    lv_obj_t *t1_freq;
    lv_obj_t *t1_band_dot;  // 2026-09-12: PropMon band-condition indicator
    lv_obj_t *t1_mode_badge;
    lv_obj_t *t1_mode_lbl;
    lv_obj_t *t1_when;
    // 2026-09-05: callsign + beam heading. 2026-09-06: split into a bold/bright
    // callsign (t1_spotted_shadow + t1_spotted_callsign, a real double-draw bold
    // effect -- this LVGL build has no separate bold font weight anywhere in the
    // codebase, only different sizes of the same regular weight, so a genuine
    // "bold" needs this technique rather than an unverified bold font asset;
    // mirrors the fake-bold double-draw trick already proven in the sibling
    // PropMon/APRSMon project) and a separate, smaller/muted beam-heading label
    // (t1_spotted_beam), per Dan's request to make the callsign specifically
    // stand out more without also emphasizing the heading text.
    lv_obj_t *t1_spotted_shadow;
    lv_obj_t *t1_spotted_callsign;
    lv_obj_t *t1_spotted_beam;
    lv_obj_t *t1_more_badge;
    lv_obj_t *t1_more_lbl;

    lv_obj_t *t2_group;
    lv_obj_t *t2_entity;
    lv_obj_t *t2_when;
    // 2026-09-06: same split as Tier 1 -- see comment above.
    lv_obj_t *t2_spotted_shadow;
    lv_obj_t *t2_spotted_callsign;
    lv_obj_t *t2_spotted_beam;

    lv_obj_t *t3_group;
    lv_obj_t *t3_count;
    lv_obj_t *t3_ticker;
};
static NeededWidgets nw;

#define MAX_TICKER_NAMES     20
#define TICKER_INTERVAL_MS   3000
static char ticker_names[MAX_TICKER_NAMES][48];
static int ticker_name_count = 0;
static int ticker_index = 0;
static uint32_t ticker_last_change_ms = 0;
static bool needed_tier3_active = false;

// ---------------------------------------------------------------------------
// New-spot flash -- 2026-09-12, revised same day per Dan's feedback: a real
// BLINK (not a solid 3-second hold), and the whole panel (not just the
// callsign) flashes, to actually catch attention from a glance rather than
// just changing a small piece of text.
//
// Design choice worth explaining: the panel's BORDER blinks green (thick
// during the "on" phase), not its background fill. A full green background
// fill was considered and rejected -- the callsign text also turns green
// while blinking, and green text on a solid green background would go
// briefly invisible during every "on" phase, working against the whole
// point of the effect. A border flash avoids that collision entirely (it's
// on the panel's edge, not behind the text) while still being a genuinely
// whole-panel, hard-to-miss effect, not just a small text-color change.
//
// Two independent targets: Watched Overview's panel + single callsign
// label, and Needed Overview's panel + Tier 1 (live) callsign pair (shadow +
// main, matching the existing double-draw bold-text technique used
// everywhere else for this pair). Needed's Tier 2 (last-seen, non-live)
// deliberately does NOT flash -- a transition into Tier 2 means a hit went
// stale/left the live buffer, not a new one arriving; any genuinely new
// spot always shows up in Tier 1 first.
//
// Real interaction with the scheduled reboot (config.h, ~every 15 min):
// without a guard, the first data fetch after every reboot would look "new"
// relative to freshly-reset in-memory state, flashing on an ordinary reboot
// even when nothing actually changed. Both *_flash_initialized flags below
// exist for exactly this reason -- only a real, in-session identity change
// (callsign + timestamp) triggers the flash, never the first populate after
// boot/reboot.
#define NEW_SPOT_FLASH_TOTAL_MS   3000  // total flash duration
#define NEW_SPOT_BLINK_HALF_MS     300  // on/off half-period -- 5 full blinks in 3s
#define FLASH_BORDER_WIDTH_ON        4
#define FLASH_BORDER_WIDTH_OFF       1  // matches make_panel()'s own default

struct FlashState {
    bool active = false;
    uint32_t until_ms = 0;        // when the whole flash sequence ends
    uint32_t next_toggle_ms = 0;  // when the next on/off toggle happens
    bool on_phase = false;        // true = currently showing the green "on" state
    lv_obj_t *panel = nullptr;              // whole panel container to border-flash
    lv_color_t panel_normal_border = COLOR_PANEL_BORDER;
    lv_obj_t *label_a = nullptr;  // primary callsign label (Watched: the only one; Needed: shadow)
    lv_obj_t *label_b = nullptr;  // secondary callsign label (Needed: main callsign); nullptr if unused
    lv_color_t normal_a = COLOR_TEXT_PRIMARY;
    lv_color_t normal_b = COLOR_TEXT_PRIMARY;
};
static FlashState watched_flash;
static FlashState needed_t1_flash;

static char watched_flash_key[64] = "";
static bool watched_flash_initialized = false;
static char needed_t1_flash_key[64] = "";
static bool needed_t1_flash_initialized = false;

/** Sets one FlashState's panel border + label(s) to either the green "on"
 * phase or their normal "off" phase. Shared by trigger_flash() (first "on")
 * and update_flash_states() (every subsequent toggle and the final revert). */
static void apply_flash_phase(FlashState &fs, bool green)
{
    if (fs.panel) {
        lv_obj_set_style_border_color(fs.panel, green ? COLOR_STATUS_GREEN : fs.panel_normal_border, 0);
        lv_obj_set_style_border_width(fs.panel, green ? FLASH_BORDER_WIDTH_ON : FLASH_BORDER_WIDTH_OFF, 0);
    }
    if (fs.label_a) {
        lv_obj_set_style_text_color(fs.label_a, green ? COLOR_STATUS_GREEN : fs.normal_a, 0);
    }
    if (fs.label_b) {
        lv_obj_set_style_text_color(fs.label_b, green ? COLOR_STATUS_GREEN : fs.normal_b, 0);
    }
}

/** Starts a blinking flash on a panel + one or two labels, remembering the
 * normal color(s)/border to return to once NEW_SPOT_FLASH_TOTAL_MS elapses.
 * Callers only invoke this after confirming a real key change -- this
 * function itself does no such check, so it always (re)starts the blink
 * when called. */
static void trigger_flash(FlashState &fs, lv_obj_t *panel, lv_color_t panel_normal_border,
                           lv_obj_t *label_a, lv_color_t normal_a,
                           lv_obj_t *label_b, lv_color_t normal_b)
{
    fs.panel = panel;
    fs.panel_normal_border = panel_normal_border;
    fs.label_a = label_a;
    fs.label_b = label_b;
    fs.normal_a = normal_a;
    fs.normal_b = normal_b;
    fs.active = true;
    fs.on_phase = true;
    uint32_t now = millis();
    fs.until_ms = now + NEW_SPOT_FLASH_TOTAL_MS;
    fs.next_toggle_ms = now + NEW_SPOT_BLINK_HALF_MS;
    apply_flash_phase(fs, true);
}

/** Called every loop() iteration (cheap early-out otherwise via the active
 * flag) -- advances each active flash's on/off blink and reverts everything
 * to normal once the total 3-second window elapses. Deliberately independent
 * of the 60s refresh cycle, since the blink needs to run on its own timer,
 * not on the next data fetch. Known, accepted minor edge case: if a Force
 * Refresh happens to land inside an active flash window (unlikely -- would
 * require tapping it within 3 seconds of a spot arriving), the refresh's own
 * re-render can interrupt the blink. Benign, not worth guarding against. */
static void update_flash_states(void)
{
    uint32_t now = millis();
    FlashState *all[] = {&watched_flash, &needed_t1_flash};
    for (FlashState *fs : all) {
        if (!fs->active) continue;
        if (now >= fs->until_ms) {
            apply_flash_phase(*fs, false);
            fs->active = false;
        } else if (now >= fs->next_toggle_ms) {
            fs->on_phase = !fs->on_phase;
            apply_flash_phase(*fs, fs->on_phase);
            fs->next_toggle_ms += NEW_SPOT_BLINK_HALF_MS;
        }
    }
}

static lv_obj_t *make_screen_overview(void)
{
    lv_obj_t *scr = make_screen();
    ov.status_text = create_header(scr, "DXMON", "LIVE");

    // Wi-Fi glyph, positioned left of the "LIVE" text using the same
    // measure-after-align technique as make_status_indicator, so it stays
    // correctly placed regardless of status text length.
    lv_obj_update_layout(ov.status_text);
    int status_x = lv_obj_get_x(ov.status_text);
    ov.wifi_glyph = lv_label_create(scr);
    lv_label_set_text(ov.wifi_glyph, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(ov.wifi_glyph, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ov.wifi_glyph, COLOR_DOT_GRAY, 0);
    lv_obj_set_pos(ov.wifi_glyph, status_x - 26, 20);

    // --- WATCHED panel (left) -- built with honest "not yet fetched" state ---
    lv_obj_t *watched = make_panel(scr, 16, 72, 378, 316);
    ov.watched_panel = watched;  // 2026-09-12: for the new-spot border flash
    // Tap-to-drill-down handled by a transparent overlay added at the end of
    // this panel's construction (see below, after all child widgets exist) --
    // a direct handler here doesn't work, since LVGL's click-testing finds
    // the deepest clickable child under the touch point first.

    make_label(watched, "WATCHED", &lv_font_montserrat_16, COLOR_ACCENT_BLUE, 20, 18);
    ov.watched_status_lbl = make_status_indicator(watched, &ov.watched_status_dot, COLOR_DOT_GRAY,
                                                   "CONNECTING", COLOR_TEXT_MUTED, 20, 18);

    ov.watched_callsign = make_label(watched, "--", &lv_font_montserrat_30, COLOR_TEXT_PRIMARY, 20, 44);
    // Added 2026-09-06: beam heading, always sent by /api/dxmon/watched as a
    // top-level "beam" field (one fixed callsign per Watched entry, so no
    // per-spot variation the way Needed has). This was purely a firmware
    // display gap -- the web /watched page has shown it since 2026-08-25.
    // Positioned inline to the callsign's right (measured after setting text
    // each refresh, same technique as Needed's own beam display) rather than
    // needing new vertical space in an already-tight panel. Styled to match
    // what was just approved for Needed: montserrat_14, bright text color.
    ov.watched_beam = make_label(watched, "", &lv_font_montserrat_14, COLOR_TEXT_PRIMARY, 0, 44);
    ov.watched_dxcc = make_label(watched, "Waiting for first fetch...", &lv_font_montserrat_16,
                                  COLOR_TEXT_SECOND, 20, 84);

    make_divider(watched, 20, 112, 338);

    make_label(watched, "FREQUENCY", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 124);
    ov.watched_freq = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, 142);
    // 2026-09-12: PropMon band-condition dot, positioned relative to the freq
    // label at render time (see update_band_dot()) since frequency text width
    // varies. Starts hidden -- shown only once real condition data arrives.
    ov.watched_band_dot = lv_obj_create(watched);
    lv_obj_remove_style_all(ov.watched_band_dot);
    lv_obj_set_size(ov.watched_band_dot, 10, 10);
    lv_obj_set_style_radius(ov.watched_band_dot, 5, 0);
    lv_obj_set_style_bg_opa(ov.watched_band_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov.watched_band_dot, LV_OBJ_FLAG_HIDDEN);

    make_label(watched, "MODE", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 164, 124);
    ov.watched_mode_badge = lv_obj_create(watched);
    lv_obj_remove_style_all(ov.watched_mode_badge);
    lv_obj_set_size(ov.watched_mode_badge, 56, 24);
    lv_obj_set_pos(ov.watched_mode_badge, 164, 142);
    lv_obj_set_style_bg_color(ov.watched_mode_badge, COLOR_BADGE_BLUE_BG, 0);
    lv_obj_set_style_bg_opa(ov.watched_mode_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ov.watched_mode_badge, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(ov.watched_mode_badge, 1, 0);
    lv_obj_set_style_radius(ov.watched_mode_badge, 6, 0);
    ov.watched_mode_lbl = lv_label_create(ov.watched_mode_badge);
    lv_label_set_text(ov.watched_mode_lbl, "--");
    lv_obj_set_style_text_font(ov.watched_mode_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ov.watched_mode_lbl, COLOR_BADGE_BLUE_TX, 0);
    lv_obj_center(ov.watched_mode_lbl);

    make_label(watched, "LAST SPOT", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 254, 124);
    ov.watched_last_spot = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 254, 142);

    make_divider(watched, 20, 174, 338);

    // Comment row -- new 2026-09-01. Fixed width + LV_LABEL_LONG_DOT truncation, unlike
    // some earlier labels, since operator comments can genuinely run long (real examples
    // seen: "FN41<F2>LR90 FT8  Sent: -11  R", "good sig into en80, multi stre") and this
    // avoids repeating the fixed-x-position clipping bug found 2026-08-29.
    make_label(watched, "COMMENT", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 184);
    ov.watched_comment = lv_label_create(watched);
    lv_label_set_text(ov.watched_comment, "");
    lv_obj_set_style_text_font(ov.watched_comment, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ov.watched_comment, COLOR_TEXT_SECOND, 0);
    lv_label_set_long_mode(ov.watched_comment, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ov.watched_comment, 338);
    lv_obj_set_pos(ov.watched_comment, 20, 200);

    make_divider(watched, 20, 225, 338);

    make_label(watched, "ACTIVE THROUGH", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 235);
    ov.watched_active_through = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_SECOND, 20, 251);

    ov.watched_badge = make_pill_badge(watched, "", 20, 278);
    ov.watched_badge_lbl = lv_obj_get_child(ov.watched_badge, 0);
    lv_obj_add_flag(ov.watched_badge, LV_OBJ_FLAG_HIDDEN);

    // Real bug found and fixed 2026-09-06: the panel-level click handler added
    // above never fired on real hardware. LVGL's lv_obj_create() widgets are
    // CLICKABLE BY DEFAULT -- this panel is packed with child containers
    // (badges, dividers, the mode-badge box) that were never explicitly marked
    // non-clickable, so touch always landed on one of THEM first and never
    // bubbled up to the panel itself. Fixed with a transparent overlay added
    // LAST (so it's on top in z-order, covering every other child underneath)
    // rather than hunting down and clearing the clickable flag on every
    // existing nested widget -- less invasive and can't miss one.
    lv_obj_t *watched_tap_overlay = lv_obj_create(watched);
    lv_obj_remove_style_all(watched_tap_overlay);
    lv_obj_set_size(watched_tap_overlay, 378, 316);
    lv_obj_set_pos(watched_tap_overlay, 0, 0);
    lv_obj_set_style_bg_opa(watched_tap_overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(watched_tap_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(watched_tap_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(watched_tap_overlay, watched_panel_click_cb, LV_EVENT_CLICKED, NULL);

    // --- NEEDED panel (right) -- real content, three-tier design agreed 2026-09-01,
    // backend (/api/dxmon/needed, including persistent last_seen) confirmed live
    // 2026-09-02; renamed/simplified from /api/dxmon/targets 2026-09-04 when Needed
    // and Wanted merged into one curated list. All three tiers' widgets are built
    // up front and toggled via LV_OBJ_FLAG_HIDDEN on their own group container,
    // rather than destroyed/rebuilt each refresh -- matches the stable, efficient
    // pattern already proven for Overview's WATCHED panel.
    lv_obj_t *needed = make_panel(scr, 406, 72, 378, 316);
    nw.panel = needed;  // 2026-09-12: for the new-spot border flash
    // Tap-to-drill-down handled by a transparent overlay added at the end of
    // this panel's construction (see below) -- same fix as WATCHED, for the
    // same reason (t1/t2/t3_group and their children are all clickable-by-
    // default lv_obj_create() widgets that fully cover the panel).
    make_label(needed, "NEEDED", &lv_font_montserrat_16, COLOR_ACCENT_AMBER, 20, 18);
    nw.status_lbl = lv_label_create(needed);
    lv_label_set_text(nw.status_lbl, "-- TRACKED");
    lv_obj_set_style_text_font(nw.status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(nw.status_lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_align(nw.status_lbl, LV_ALIGN_TOP_RIGHT, -20, 20);
    make_divider(needed, 20, 46, 338);

    // Tier 1 -- something has a real live spot right now.
    nw.t1_group = lv_obj_create(needed);
    lv_obj_remove_style_all(nw.t1_group);
    lv_obj_set_pos(nw.t1_group, 0, 0);
    lv_obj_set_size(nw.t1_group, 378, 316);
    lv_obj_clear_flag(nw.t1_group, LV_OBJ_FLAG_SCROLLABLE);

    nw.t1_type_badge = lv_obj_create(nw.t1_group);
    lv_obj_remove_style_all(nw.t1_type_badge);
    lv_obj_set_size(nw.t1_type_badge, 70, 22);
    lv_obj_set_pos(nw.t1_type_badge, 20, 58);
    lv_obj_set_style_radius(nw.t1_type_badge, 6, 0);
    lv_obj_set_style_border_width(nw.t1_type_badge, 1, 0);
    nw.t1_type_lbl = lv_label_create(nw.t1_type_badge);
    lv_obj_set_style_text_font(nw.t1_type_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(nw.t1_type_lbl);

    nw.t1_entity = lv_label_create(nw.t1_group);
    lv_label_set_text(nw.t1_entity, "--");
    lv_obj_set_style_text_font(nw.t1_entity, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(nw.t1_entity, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(nw.t1_entity, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nw.t1_entity, 338);
    lv_obj_set_pos(nw.t1_entity, 20, 88);

    nw.t1_subtitle = lv_label_create(nw.t1_group);
    lv_label_set_text(nw.t1_subtitle, "");
    lv_obj_set_style_text_font(nw.t1_subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nw.t1_subtitle, COLOR_TEXT_SECOND, 0);
    lv_obj_set_pos(nw.t1_subtitle, 20, 110);

    make_divider(nw.t1_group, 20, 138, 338);

    make_label(nw.t1_group, "FREQUENCY", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 150);
    nw.t1_freq = make_label(nw.t1_group, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, 168);
    // 2026-09-12: PropMon band-condition dot -- same technique as Watched's own,
    // see comment there.
    nw.t1_band_dot = lv_obj_create(nw.t1_group);
    lv_obj_remove_style_all(nw.t1_band_dot);
    lv_obj_set_size(nw.t1_band_dot, 10, 10);
    lv_obj_set_style_radius(nw.t1_band_dot, 5, 0);
    lv_obj_set_style_bg_opa(nw.t1_band_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(nw.t1_band_dot, LV_OBJ_FLAG_HIDDEN);

    make_label(nw.t1_group, "MODE", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 164, 150);
    nw.t1_mode_badge = lv_obj_create(nw.t1_group);
    lv_obj_remove_style_all(nw.t1_mode_badge);
    lv_obj_set_size(nw.t1_mode_badge, 56, 24);
    lv_obj_set_pos(nw.t1_mode_badge, 164, 168);
    lv_obj_set_style_bg_color(nw.t1_mode_badge, COLOR_BADGE_BLUE_BG, 0);
    lv_obj_set_style_bg_opa(nw.t1_mode_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nw.t1_mode_badge, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(nw.t1_mode_badge, 1, 0);
    lv_obj_set_style_radius(nw.t1_mode_badge, 6, 0);
    nw.t1_mode_lbl = lv_label_create(nw.t1_mode_badge);
    lv_label_set_text(nw.t1_mode_lbl, "--");
    lv_obj_set_style_text_font(nw.t1_mode_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nw.t1_mode_lbl, COLOR_BADGE_BLUE_TX, 0);
    lv_obj_center(nw.t1_mode_lbl);

    make_label(nw.t1_group, "LAST SPOT", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 254, 150);
    nw.t1_when = make_label(nw.t1_group, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 254, 168);

    // Added 2026-09-05: the spotted callsign (and its beam heading, when the
    // lookup succeeds) -- real, useful information Needed's own entity/slot
    // targets don't otherwise show anywhere, unlike Watched where the callsign
    // IS the entry itself.
    // 2026-09-06: callsign made more prominent per Dan's request, then enlarged
    // further the same day -- now montserrat_26 (matching the size already
    // proven for Watched's own roster-row callsigns), NEEDED's own bright amber
    // accent color, and a real double-draw bold effect (shadow copy 1px right,
    // same text/color/font, drawn first so the main label overlaps it). The
    // beam suffix sits inline to the callsign's right rather than on its own
    // line, positioned in update_overview_needed() once the callsign's real
    // rendered width is known each refresh (same measure-then-align technique
    // as make_status_indicator()) -- keeps this to one line despite the much
    // taller font, so the "+N more tracked" badge only needs to move down
    // once, not twice.
    nw.t1_spotted_shadow = make_label(nw.t1_group, "", &lv_font_montserrat_26, COLOR_ACCENT_AMBER, 21, 196);
    nw.t1_spotted_callsign = make_label(nw.t1_group, "", &lv_font_montserrat_26, COLOR_ACCENT_AMBER, 20, 196);
    nw.t1_spotted_beam = make_label(nw.t1_group, "", &lv_font_montserrat_14, COLOR_TEXT_PRIMARY, 20, 196);

    nw.t1_more_badge = make_pill_badge(nw.t1_group, "", 20, 254);
    nw.t1_more_lbl = lv_obj_get_child(nw.t1_more_badge, 0);
    lv_obj_add_flag(nw.t1_more_badge, LV_OBJ_FLAG_HIDDEN);

    // Real bug found and fixed 2026-09-03: t1_group was never hidden by default, so
    // before the first successful fetch it showed raw, un-set-up LVGL default widget
    // content (literally the word "Text", "--" everywhere) -- confirmed directly from
    // a real hardware video. Hidden here now, same as t2/t3 already were, with a
    // proper loading group taking its place until real data arrives.
    lv_obj_add_flag(nw.t1_group, LV_OBJ_FLAG_HIDDEN);

    // Tier 2 -- no live spot anywhere, but real history exists.
    nw.t2_group = lv_obj_create(needed);
    lv_obj_remove_style_all(nw.t2_group);
    lv_obj_set_pos(nw.t2_group, 0, 0);
    lv_obj_set_size(nw.t2_group, 378, 316);
    lv_obj_clear_flag(nw.t2_group, LV_OBJ_FLAG_SCROLLABLE);

    make_label(nw.t2_group, "LAST HIT", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 110);
    nw.t2_entity = lv_label_create(nw.t2_group);
    lv_label_set_text(nw.t2_entity, "--");
    lv_obj_set_style_text_font(nw.t2_entity, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(nw.t2_entity, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(nw.t2_entity, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nw.t2_entity, 338);
    lv_obj_set_pos(nw.t2_entity, 20, 130);
    nw.t2_when = make_label(nw.t2_group, "--", &lv_font_montserrat_14, COLOR_TEXT_SECOND, 20, 156);
    // 2026-09-06: same enlarged treatment as Tier 1 -- see comment there.
    nw.t2_spotted_shadow = make_label(nw.t2_group, "", &lv_font_montserrat_26, COLOR_ACCENT_AMBER, 21, 190);
    nw.t2_spotted_callsign = make_label(nw.t2_group, "", &lv_font_montserrat_26, COLOR_ACCENT_AMBER, 20, 190);
    nw.t2_spotted_beam = make_label(nw.t2_group, "", &lv_font_montserrat_14, COLOR_TEXT_PRIMARY, 20, 190);
    lv_obj_add_flag(nw.t2_group, LV_OBJ_FLAG_HIDDEN);

    // Tier 3 -- cold start, nothing's ever hit. Count anchor + rotating ticker, per
    // Dan's own reaction 2026-09-01: "that will keep it interesting."
    nw.t3_group = lv_obj_create(needed);
    lv_obj_remove_style_all(nw.t3_group);
    lv_obj_set_pos(nw.t3_group, 0, 0);
    lv_obj_set_size(nw.t3_group, 378, 316);
    lv_obj_clear_flag(nw.t3_group, LV_OBJ_FLAG_SCROLLABLE);

    nw.t3_count = make_label(nw.t3_group, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, 118);
    nw.t3_ticker = make_label(nw.t3_group, "", &lv_font_montserrat_14, COLOR_TEXT_MUTED, 20, 144);
    lv_obj_add_flag(nw.t3_group, LV_OBJ_FLAG_HIDDEN);

    // Default loading state -- visible until the first real update_overview_needed()
    // call, mirroring the WATCHED panel's own honest "Waiting for first fetch..."
    // pre-fetch treatment rather than showing nothing (or raw default widgets).
    nw.loading_lbl = lv_label_create(needed);
    lv_label_set_text(nw.loading_lbl, "Waiting for first fetch...");
    lv_obj_set_style_text_font(nw.loading_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nw.loading_lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_set_pos(nw.loading_lbl, 20, 88);

    // 2026-09-06: same transparent-overlay fix as the WATCHED panel -- added
    // last, so it sits on top of t1_group/t2_group/t3_group/loading_lbl in
    // z-order regardless of which one is currently visible.
    lv_obj_t *needed_tap_overlay = lv_obj_create(needed);
    lv_obj_remove_style_all(needed_tap_overlay);
    lv_obj_set_size(needed_tap_overlay, 378, 316);
    lv_obj_set_pos(needed_tap_overlay, 0, 0);
    lv_obj_set_style_bg_opa(needed_tap_overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(needed_tap_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(needed_tap_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(needed_tap_overlay, needed_panel_click_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

static int select_featured_entry(const WatchedData &data)
{
    if (data.count == 0) return -1;
    int best = -1;
    for (int i = 0; i < data.count; i++) {
        if (!data.entries[i].has_last_spot) continue;
        if (best == -1 || strcmp(data.entries[i].received_at, data.entries[best].received_at) > 0) {
            best = i;
        }
    }
    return (best != -1) ? best : 0;
}

static void update_overview_watched(const WatchedData &data)
{
    if (data.count == 0) {
        lv_label_set_text(ov.watched_status_lbl, "NO DATA");
        lv_obj_set_style_bg_color(ov.watched_status_dot, COLOR_DOT_GRAY, 0);
        lv_label_set_text(ov.watched_callsign, "--");
        lv_label_set_text(ov.watched_beam, "");
        lv_label_set_text(ov.watched_dxcc, "Watchlist is empty");
        lv_label_set_text(ov.watched_freq, "--");
        lv_obj_add_flag(ov.watched_band_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ov.watched_mode_lbl, "--");
        lv_label_set_text(ov.watched_last_spot, "--");
        lv_label_set_text(ov.watched_comment, "");
        lv_label_set_text(ov.watched_active_through, "--");
        lv_obj_add_flag(ov.watched_badge, LV_OBJ_FLAG_HIDDEN);
        reposition_status_indicator(ov.watched_status_lbl, ov.watched_status_dot);
        return;
    }

    int idx = select_featured_entry(data);
    const WatchedEntry &e = data.entries[idx];

    if (e.has_last_spot) {
        lv_label_set_text(ov.watched_status_lbl, "ACTIVE");
        lv_obj_set_style_text_color(ov.watched_status_lbl, COLOR_STATUS_GREEN, 0);
        lv_obj_set_style_bg_color(ov.watched_status_dot, COLOR_STATUS_GREEN, 0);
    } else if (e.adxo_active) {
        lv_label_set_text(ov.watched_status_lbl, "WAITING");
        lv_obj_set_style_text_color(ov.watched_status_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_bg_color(ov.watched_status_dot, COLOR_DOT_GRAY, 0);
    } else {
        lv_label_set_text(ov.watched_status_lbl, "-");
        lv_obj_set_style_text_color(ov.watched_status_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_bg_color(ov.watched_status_dot, COLOR_DOT_GRAY, 0);
    }
    reposition_status_indicator(ov.watched_status_lbl, ov.watched_status_dot);

    lv_label_set_text(ov.watched_callsign, e.callsign);
    lv_label_set_text(ov.watched_dxcc, e.dxcc);

    // 2026-09-06: beam heading, inline to the callsign's right -- format_beam_suffix()
    // already exists (built for Needed, 2026-09-05), reused here unchanged.
    char watched_beam_buf[32];
    format_beam_suffix(e, watched_beam_buf, sizeof(watched_beam_buf));
    lv_label_set_text(ov.watched_beam, watched_beam_buf);
    lv_obj_update_layout(ov.watched_callsign);
    lv_obj_align_to(ov.watched_beam, ov.watched_callsign, LV_ALIGN_OUT_RIGHT_MID, 12, 5);

    if (e.has_last_spot) {
        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", e.frequency);
        lv_label_set_text(ov.watched_freq, freq_buf);
        // 2026-09-12: PropMon band-condition dot -- see update_band_dot() comment.
        update_band_dot(ov.watched_band_dot, ov.watched_freq, e.band_condition);

        char mode_buf[16];
        strncpy(mode_buf, e.mode, sizeof(mode_buf) - 1);
        mode_buf[sizeof(mode_buf) - 1] = '\0';
        to_upper_inplace(mode_buf);
        lv_label_set_text(ov.watched_mode_lbl, mode_buf);

        char when_buf[24];
        format_short_datetime(e.received_at, when_buf, sizeof(when_buf));
        lv_label_set_text(ov.watched_last_spot, when_buf);

        if (e.comment[0] != '\0') {
            char comment_buf[80];
            snprintf(comment_buf, sizeof(comment_buf), "\"%s\"", e.comment);
            lv_label_set_text(ov.watched_comment, comment_buf);
        } else {
            lv_label_set_text(ov.watched_comment, "");
        }
    } else {
        lv_label_set_text(ov.watched_freq, "--");
        lv_obj_add_flag(ov.watched_band_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ov.watched_mode_lbl, "--");
        lv_label_set_text(ov.watched_last_spot, "Not yet spotted");
        lv_label_set_text(ov.watched_comment, "");
    }

    // 2026-09-12: new-spot flash detection -- see FlashState comment near the
    // top of this file for the full reasoning. Only fires when a live spot's
    // identity (callsign + timestamp) is genuinely different from what was
    // last shown here -- not on the routine 60s re-fetch of the same still-
    // active spot, not on the very first populate after boot/reboot, and not
    // when a spot simply ages out of the live buffer (has_last_spot -> false).
    {
        char new_key[64];
        if (e.has_last_spot) {
            snprintf(new_key, sizeof(new_key), "%s|%s", e.callsign, e.received_at);
        } else {
            new_key[0] = '\0';
        }
        if (watched_flash_initialized && new_key[0] != '\0' &&
            strcmp(new_key, watched_flash_key) != 0) {
            trigger_flash(watched_flash, ov.watched_panel, COLOR_PANEL_BORDER,
                          ov.watched_callsign, COLOR_TEXT_PRIMARY, nullptr, COLOR_TEXT_PRIMARY);
        }
        strncpy(watched_flash_key, new_key, sizeof(watched_flash_key) - 1);
        watched_flash_key[sizeof(watched_flash_key) - 1] = '\0';
        watched_flash_initialized = true;
    }

    char through_buf[24];
    format_short_date(e.adxo_end, through_buf, sizeof(through_buf));
    lv_label_set_text(ov.watched_active_through, through_buf);

    int more = data.count - 1;
    if (more > 0) {
        char badge_buf[32];
        snprintf(badge_buf, sizeof(badge_buf), "+%d more watched", more);
        lv_label_set_text(ov.watched_badge_lbl, badge_buf);
        lv_obj_clear_flag(ov.watched_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ov.watched_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

/** Wi-Fi glyph reflects real-time WiFi.status() -- no server round-trip needed. */
static void update_wifi_glyph(void)
{
    bool connected = (WiFi.status() == WL_CONNECTED);
    lv_obj_set_style_text_color(ov.wifi_glyph, connected ? COLOR_STATUS_GREEN : COLOR_DOT_GRAY, 0);
}

// ---------------------------------------------------------------------------
// NEEDED panel (Overview) -- three-tier display over /api/dxmon/needed, Dan's
// unified curated list (renamed/simplified 2026-09-04 from the old
// /api/dxmon/targets, which cross-referenced all of no_confirms.csv against
// ADXO + live spots). Tier chosen fresh on every fetch:
//   1. Something has a real live spot right now (last_spot present on any entry)
//      -> feature it, mirroring Watched's own tier-1 treatment.
//   2. Nothing live, but at least one entry has persisted last_seen data
//      -> "Last hit: <date> -- <entity>".
//   3. Nothing has ever hit -> a tracked-count anchor line + a slow rotating
//      ticker through real tracked entity names, so a quiet feature never
//      looks broken. See the 2026-09-01 design discussion and the 2026-09-02
//      backend work (persistent last_seen) that made Tier 2 possible at all.
// ---------------------------------------------------------------------------

static int select_featured_target(const NeededData &data, bool want_live)
{
    int best = -1;
    for (int i = 0; i < data.count; i++) {
        const SpotInfo &spot = want_live ? data.entries[i].last_spot : data.entries[i].last_seen;
        if (!spot.present) continue;
        if (best == -1 || strcmp(spot.received_at, (want_live ? data.entries[best].last_spot : data.entries[best].last_seen).received_at) > 0) {
            best = i;
        }
    }
    return best;
}

static void populate_ticker(const NeededData &data)
{
    ticker_name_count = 0;
    for (int i = 0; i < data.count && ticker_name_count < MAX_TICKER_NAMES; i++) {
        strncpy(ticker_names[ticker_name_count], data.entries[i].entity, sizeof(ticker_names[0]) - 1);
        ticker_names[ticker_name_count][sizeof(ticker_names[0]) - 1] = '\0';
        ticker_name_count++;
    }
    ticker_index = 0;
    ticker_last_change_ms = millis();
    if (ticker_name_count > 0) {
        lv_label_set_text(nw.t3_ticker, ticker_names[0]);
    } else {
        lv_label_set_text(nw.t3_ticker, "");
    }
}

/** Called every loop() iteration; only actually updates when Tier 3 is showing and the
 * interval has elapsed -- cheap check otherwise. */
static void advance_ticker_if_needed(void)
{
    if (!needed_tier3_active || ticker_name_count == 0) return;
    if (millis() - ticker_last_change_ms < TICKER_INTERVAL_MS) return;
    ticker_index = (ticker_index + 1) % ticker_name_count;
    lv_label_set_text(nw.t3_ticker, ticker_names[ticker_index]);
    ticker_last_change_ms = millis();
}

static void update_overview_needed(const NeededData &data)
{
    lv_obj_add_flag(nw.loading_lbl, LV_OBJ_FLAG_HIDDEN);

    // 2026-09-04: "kind" replaces the old "type" (needed/wanted) field -- an entry
    // is "slot" when it has a specific band/mode, "entity" otherwise (whole-entity
    // target). Same idea as before, renamed to match the unified list's own terms.
    int entity_count = 0, slot_count = 0;
    for (int i = 0; i < data.count; i++) {
        if (strcmp(data.entries[i].kind, "slot") == 0) slot_count++;
        else entity_count++;
    }
    char status_buf[24];
    snprintf(status_buf, sizeof(status_buf), "%d TRACKED", data.count);
    lv_label_set_text(nw.status_lbl, status_buf);

    int live_idx = select_featured_target(data, true);

    if (live_idx != -1) {
        needed_tier3_active = false;
        lv_obj_clear_flag(nw.t1_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nw.t2_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nw.t3_group, LV_OBJ_FLAG_HIDDEN);

        const NeededEntry &t = data.entries[live_idx];
        bool is_slot = (strcmp(t.kind, "slot") == 0);

        lv_label_set_text(nw.t1_type_lbl, is_slot ? "SLOT" : "ENTITY");
        lv_obj_set_style_bg_color(nw.t1_type_badge, is_slot ? COLOR_BADGE_BLUE_BG : lv_color_hex(0x332a10), 0);
        lv_obj_set_style_bg_opa(nw.t1_type_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(nw.t1_type_badge, is_slot ? COLOR_ACCENT_BLUE : COLOR_ACCENT_AMBER, 0);
        lv_obj_set_style_text_color(nw.t1_type_lbl, is_slot ? COLOR_BADGE_BLUE_TX : COLOR_ACCENT_AMBER, 0);

        lv_label_set_text(nw.t1_entity, t.entity);

        // Subtitle: band/mode for a slot target. Entity-kind targets have no
        // equivalent subtitle now that the old CSV "prefix" field is gone (that
        // was specific to the retired no_confirms.csv cross-reference) -- left
        // blank rather than showing something misleading.
        if (is_slot) {
            char sub_buf[48];
            snprintf(sub_buf, sizeof(sub_buf), "%s%s%s", t.band, (t.band[0] && t.mode[0]) ? " " : "", t.mode);
            lv_label_set_text(nw.t1_subtitle, sub_buf);
        } else {
            lv_label_set_text(nw.t1_subtitle, "");
        }

        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", t.last_spot.frequency);
        lv_label_set_text(nw.t1_freq, freq_buf);
        // 2026-09-12: PropMon band-condition dot -- see update_band_dot() comment
        // near the top of this file.
        update_band_dot(nw.t1_band_dot, nw.t1_freq, t.last_spot.band_condition);

        char mode_buf[16];
        strncpy(mode_buf, t.last_spot.mode, sizeof(mode_buf) - 1);
        mode_buf[sizeof(mode_buf) - 1] = '\0';
        to_upper_inplace(mode_buf);
        lv_label_set_text(nw.t1_mode_lbl, mode_buf);

        char when_buf[24];
        format_short_datetime(t.last_spot.received_at, when_buf, sizeof(when_buf));
        lv_label_set_text(nw.t1_when, when_buf);

        // 2026-09-06: callsign and beam heading set as two separate labels now
        // (see the NeededWidgets struct comment) -- shadow and main callsign
        // labels get the same text for the double-draw bold effect. The beam
        // suffix is positioned inline to the callsign's right each refresh,
        // since different callsigns render at different widths.
        // 2026-09-08: dims to muted when the spot is more than 4 hours old,
        // per Dan's own real operational point -- an old spot has almost
        // certainly moved bands/modes or gone quiet by then.
        bool t1_stale = spot_is_stale(t.last_spot.received_at, data.updated);
        lv_color_t t1_cs_color = stale_aware_color(t1_stale, COLOR_ACCENT_AMBER);
        lv_label_set_text(nw.t1_spotted_shadow, t.last_spot.callsign);
        lv_obj_set_style_text_color(nw.t1_spotted_shadow, t1_cs_color, 0);
        lv_label_set_text(nw.t1_spotted_callsign, t.last_spot.callsign);
        lv_obj_set_style_text_color(nw.t1_spotted_callsign, t1_cs_color, 0);

        // 2026-09-12: new-spot flash detection -- see FlashState comment near
        // the top of this file. This branch only ever runs while Tier 1 (a
        // real live hit) is active, so any key change here is always a
        // meaningful new-arrival event, not a fade-to-Tier-2 transition
        // (that's handled by resetting needed_t1_flash_key to "" wherever
        // Tier 1 is NOT active, below and in Tier 2/3, so the next real
        // return to Tier 1 is correctly treated as new).
        {
            char new_key[64];
            snprintf(new_key, sizeof(new_key), "%s|%s", t.last_spot.callsign, t.last_spot.received_at);
            if (needed_t1_flash_initialized && strcmp(new_key, needed_t1_flash_key) != 0) {
                trigger_flash(needed_t1_flash, nw.panel, COLOR_PANEL_BORDER,
                              nw.t1_spotted_shadow, t1_cs_color,
                              nw.t1_spotted_callsign, t1_cs_color);
            }
            strncpy(needed_t1_flash_key, new_key, sizeof(needed_t1_flash_key) - 1);
            needed_t1_flash_key[sizeof(needed_t1_flash_key) - 1] = '\0';
            needed_t1_flash_initialized = true;
        }

        char beam_buf[32];
        format_beam_suffix(t.last_spot, beam_buf, sizeof(beam_buf));
        lv_label_set_text(nw.t1_spotted_beam, beam_buf);
        lv_obj_update_layout(nw.t1_spotted_callsign);
        lv_obj_align_to(nw.t1_spotted_beam, nw.t1_spotted_callsign, LV_ALIGN_OUT_RIGHT_MID, 10, 3);

        int more = data.count - 1;
        if (more > 0) {
            char more_buf[32];
            snprintf(more_buf, sizeof(more_buf), "+%d more tracked", more);
            lv_label_set_text(nw.t1_more_lbl, more_buf);
            lv_obj_clear_flag(nw.t1_more_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(nw.t1_more_badge, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    int seen_idx = select_featured_target(data, false);
    if (seen_idx != -1) {
        needed_tier3_active = false;
        lv_obj_add_flag(nw.t1_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nw.t2_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nw.t3_group, LV_OBJ_FLAG_HIDDEN);
        // Tier 1 not active -- reset so the next real return to Tier 1 is
        // correctly treated as a new arrival, not a repeat. See the flash
        // detection comment in the Tier 1 branch above.
        needed_t1_flash_key[0] = '\0';

        const NeededEntry &t = data.entries[seen_idx];
        lv_label_set_text(nw.t2_entity, t.entity);
        char when_buf[24];
        char line_buf[40];
        format_short_datetime(t.last_seen.received_at, when_buf, sizeof(when_buf));
        snprintf(line_buf, sizeof(line_buf), "Last hit: %s", when_buf);
        lv_label_set_text(nw.t2_when, line_buf);

        // 2026-09-06: same split and inline-alignment as Tier 1 -- see comment there.
        // 2026-09-08: same staleness dimming as Tier 1 -- Tier 2 exists
        // specifically to show the last known hit when nothing's currently
        // live, so it's often going to be the more likely of the two tiers
        // to actually cross the staleness threshold in practice. Dims the
        // "Last hit" timestamp line too, not just the callsign/beam.
        bool t2_stale = spot_is_stale(t.last_seen.received_at, data.updated);
        lv_obj_set_style_text_color(nw.t2_when, stale_aware_color(t2_stale, COLOR_TEXT_SECOND), 0);
        lv_color_t t2_cs_color = stale_aware_color(t2_stale, COLOR_ACCENT_AMBER);
        lv_label_set_text(nw.t2_spotted_shadow, t.last_seen.callsign);
        lv_obj_set_style_text_color(nw.t2_spotted_shadow, t2_cs_color, 0);
        lv_label_set_text(nw.t2_spotted_callsign, t.last_seen.callsign);
        lv_obj_set_style_text_color(nw.t2_spotted_callsign, t2_cs_color, 0);
        char beam_buf[32];
        format_beam_suffix(t.last_seen, beam_buf, sizeof(beam_buf));
        lv_label_set_text(nw.t2_spotted_beam, beam_buf);
        lv_obj_update_layout(nw.t2_spotted_callsign);
        lv_obj_align_to(nw.t2_spotted_beam, nw.t2_spotted_callsign, LV_ALIGN_OUT_RIGHT_MID, 10, 3);
        return;
    }

    // Tier 3 -- cold start.
    lv_obj_add_flag(nw.t1_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nw.t2_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nw.t3_group, LV_OBJ_FLAG_HIDDEN);
    // Tier 1 not active -- see reset comment in the Tier 2 branch above.
    needed_t1_flash_key[0] = '\0';

    if (data.count == 0) {
        // Added 2026-09-08: a genuinely empty curated list is a different
        // real state from "entries exist, none spotted yet" -- Dan's own
        // request, matching Watched's existing dedicated "Watchlist is
        // empty" message, but going further with an actual nudge toward the
        // curation page rather than just stating the list is empty. Reuses
        // the same two Tier 3 widgets (count/ticker slots) rather than
        // adding new ones, since it's the same visual layout either way.
        lv_label_set_text(nw.t3_count, "Needed list is empty");
        char nudge_buf[64];
        snprintf(nudge_buf, sizeof(nudge_buf), "Curate entries at %s:%d/needed",
                 DXMON_SERVER_HOST, DXMON_SERVER_PORT);
        lv_label_set_text(nw.t3_ticker, nudge_buf);
    } else {
        char count_buf[40];
        snprintf(count_buf, sizeof(count_buf), "%d entity - %d slot tracked", entity_count, slot_count);
        lv_label_set_text(nw.t3_count, count_buf);
        populate_ticker(data);
    }
    needed_tier3_active = true;
}

// ---------------------------------------------------------------------------
// Config -- real status panel (Wi-Fi, ADXO poll, HamAlert Telnet, watched
// count, curate-at URL, firmware version) + Force Refresh. Wi-Fi Setup is a
// visible but disabled placeholder -- a real captive-portal reconfiguration
// flow is out of scope for this pass (see session notes: both PropMon and
// APRSMon needed multiple dedicated sessions to build that feature safely).
// ---------------------------------------------------------------------------
struct ConfigWidgets {
    lv_obj_t *wifi_lbl;
    lv_obj_t *wifi_dot;
    lv_obj_t *adxo_lbl;
    lv_obj_t *adxo_dot;
    lv_obj_t *hamalert_lbl;
    lv_obj_t *hamalert_dot;
    lv_obj_t *watchlist_lbl;
};
static ConfigWidgets cfgw;
static volatile bool force_refresh_requested = false;

static void force_refresh_btn_cb(lv_event_t *e)
{
    force_refresh_requested = true;
}

static lv_obj_t *make_config_row(lv_obj_t *parent, lv_obj_t **out_dot, const char *label,
                                  const char *initial_value, lv_color_t value_color, int y)
{
    make_label(parent, label, &lv_font_montserrat_16, COLOR_TEXT_SECOND, 40, y);
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, COLOR_DOT_GRAY, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, 20, y + 4);
    if (out_dot) *out_dot = dot;

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, initial_value);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val, value_color, 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -24, y - 4);
    return val;
}

static lv_obj_t *make_screen_config(void)
{
    lv_obj_t *scr = make_screen();
    create_header(scr, "CONFIG", "v1.0");

    lv_obj_t *panel = make_panel(scr, 16, 72, 768, 240);

    cfgw.wifi_lbl = make_config_row(panel, &cfgw.wifi_dot, "Wi-Fi", "Connecting...", COLOR_TEXT_MUTED, 30);
    make_divider(panel, 20, 50, 728);

    cfgw.adxo_lbl = make_config_row(panel, &cfgw.adxo_dot, "ADXO Poll", "--", COLOR_TEXT_MUTED, 70);
    make_divider(panel, 20, 90, 728);

    cfgw.hamalert_lbl = make_config_row(panel, &cfgw.hamalert_dot, "HamAlert Telnet", "--", COLOR_TEXT_MUTED, 110);
    make_divider(panel, 20, 130, 728);

    cfgw.watchlist_lbl = make_config_row(panel, nullptr, "Watchlist", "--", COLOR_TEXT_PRIMARY, 150);
    make_divider(panel, 20, 170, 728);

    char curate_buf[48];
    snprintf(curate_buf, sizeof(curate_buf), "http://%s:%d", DXMON_SERVER_HOST, DXMON_SERVER_PORT);
    make_config_row(panel, nullptr, "Curate at", curate_buf, COLOR_BADGE_BLUE_TX, 190);
    make_divider(panel, 20, 210, 728);

    make_config_row(panel, nullptr, "Firmware", "v1.0", COLOR_TEXT_PRIMARY, 227);

    // Force Refresh -- real, functional.
    lv_obj_t *refresh_btn = lv_btn_create(scr);
    lv_obj_set_size(refresh_btn, 368, 66);
    lv_obj_set_pos(refresh_btn, 16, 330);
    lv_obj_set_style_bg_color(refresh_btn, COLOR_BADGE_BLUE_BG, 0);
    lv_obj_set_style_border_color(refresh_btn, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(refresh_btn, 2, 0);
    lv_obj_set_style_radius(refresh_btn, 10, 0);
    lv_obj_add_event_cb(refresh_btn, force_refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, "Force Refresh");
    lv_obj_set_style_text_font(refresh_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(refresh_lbl, COLOR_BADGE_BLUE_TX, 0);
    lv_obj_center(refresh_lbl);

    // Wi-Fi Setup -- visible, deliberately disabled placeholder (see comment
    // above the struct). Muted styling distinguishes it from the real button.
    lv_obj_t *setup_btn = lv_obj_create(scr);
    lv_obj_remove_style_all(setup_btn);
    lv_obj_set_size(setup_btn, 368, 66);
    lv_obj_set_pos(setup_btn, 408, 330);
    lv_obj_set_style_bg_color(setup_btn, COLOR_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(setup_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(setup_btn, COLOR_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(setup_btn, 2, 0);
    lv_obj_set_style_radius(setup_btn, 10, 0);
    lv_obj_t *setup_lbl = lv_label_create(setup_btn);
    lv_label_set_text(setup_lbl, "Wi-Fi Setup (coming soon)");
    lv_obj_set_style_text_font(setup_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(setup_lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_center(setup_lbl);

    return scr;
}

/** Updates Wi-Fi row from real-time WiFi.status()/localIP() -- no fetch needed. */
static void update_config_wifi(void)
{
    if (WiFi.status() == WL_CONNECTED) {
        char buf[48];
        IPAddress ip = WiFi.localIP();
        snprintf(buf, sizeof(buf), "Connected %s %d.%d.%d.%d", "\xE2\x80\xA2", ip[0], ip[1], ip[2], ip[3]);
        lv_label_set_text(cfgw.wifi_lbl, buf);
        lv_obj_set_style_text_color(cfgw.wifi_lbl, COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_color(cfgw.wifi_dot, COLOR_STATUS_GREEN, 0);
    } else {
        lv_label_set_text(cfgw.wifi_lbl, "Disconnected");
        lv_obj_set_style_text_color(cfgw.wifi_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_bg_color(cfgw.wifi_dot, COLOR_DOT_GRAY, 0);
    }
}

static void update_config_preview_status(const PreviewStatus &ps)
{
    char adxo_buf[48];
    char when_buf[24];
    format_short_datetime(ps.adxo_updated, when_buf, sizeof(when_buf));
    snprintf(adxo_buf, sizeof(adxo_buf), "OK %s %s", "\xE2\x80\xA2", when_buf);
    lv_label_set_text(cfgw.adxo_lbl, adxo_buf);
    lv_obj_set_style_text_color(cfgw.adxo_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(cfgw.adxo_dot, COLOR_STATUS_GREEN, 0);

    // HamAlert -- no last-spot timestamp at this endpoint (see struct comment
    // in dxmon_client.h); shows connection state only.
    if (!ps.hamalert_enabled) {
        lv_label_set_text(cfgw.hamalert_lbl, "Disabled");
        lv_obj_set_style_text_color(cfgw.hamalert_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_bg_color(cfgw.hamalert_dot, COLOR_DOT_GRAY, 0);
    } else if (ps.hamalert_connected && ps.hamalert_logged_in) {
        lv_label_set_text(cfgw.hamalert_lbl, "Connected");
        lv_obj_set_style_text_color(cfgw.hamalert_lbl, COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_color(cfgw.hamalert_dot, COLOR_STATUS_GREEN, 0);
    } else {
        lv_label_set_text(cfgw.hamalert_lbl, "Disconnected");
        lv_obj_set_style_text_color(cfgw.hamalert_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_bg_color(cfgw.hamalert_dot, COLOR_DOT_GRAY, 0);
    }

    char watch_buf[24];
    snprintf(watch_buf, sizeof(watch_buf), "%d watched", ps.watched_count);
    lv_label_set_text(cfgw.watchlist_lbl, watch_buf);
}

// ---------------------------------------------------------------------------
// Honest placeholders -- Watched/Needed tabs not yet built.
// ---------------------------------------------------------------------------
static lv_obj_t *make_screen_placeholder(const char *title, const char *status)
{
    lv_obj_t *scr = make_screen();
    create_header(scr, title, status);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Not yet built");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

    return scr;
}

// ---------------------------------------------------------------------------
// Bottom tab bar -- four tabs, text-only for now (mockup's custom icon
// glyphs deferred as a visual-polish item).
// ---------------------------------------------------------------------------
static lv_obj_t *screens[4];
static const char *tab_names[4] = {"Overview", "Watched", "Needed", "Config"};

static void tab_btn_event_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    lv_scr_load(screens[index]);
}

static void create_tab_bar(lv_obj_t *parent, int active_index)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, 64);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    make_divider(bar, 0, 0, 800);

    lv_obj_t *indicator = lv_obj_create(bar);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, 200, 4);
    lv_obj_set_pos(indicator, active_index * 200, 0);
    lv_obj_set_style_bg_color(indicator, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 200, 64);
        lv_obj_set_pos(btn, i * 200, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(btn, tab_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, i == active_index ? COLOR_TEXT_PRIMARY : COLOR_TEXT_MUTED, 0);
        lv_obj_center(lbl);
    }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
static uint32_t last_fetch_ms = 0;

// ---------------------------------------------------------------------------
// Watched roster (tab bar destination) -- full scrollable list of every
// watched entry, built from the approved dxmon_watched_mockup.svg. Real
// content, replacing the "0 TRACKED" placeholder.
// ---------------------------------------------------------------------------

/**
 * Howard Hinnant's well-known public-domain "days from civil" algorithm --
 * converts a Y/M/D date to a day-count (proleptic Gregorian), correct across
 * month/year/leap-year boundaries. Used only for a one-shot day-difference
 * computation per fetch (comparing adxo.begin against the server's own
 * "updated" timestamp) -- not live elapsed-time tracking, so no NTP/RTC is
 * needed on the device, per the 2026-09-01 design discussion.
 */
static long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static bool parse_iso_ymd(const char *iso, int *y, int *m, int *d)
{
    if (!iso || strlen(iso) < 10) return false;
    *y = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 + (iso[3] - '0');
    *m = (iso[5] - '0') * 10 + (iso[6] - '0');
    *d = (iso[8] - '0') * 10 + (iso[9] - '0');
    return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= 31);
}

/**
 * "Starts in N days" (or "Starts today"/"Starts tomorrow" for the near
 * cases, which read far more naturally than "in 0 days"/"in 1 days") as the
 * primary line, plus the plain exact date as a smaller secondary line --
 * per Dan's request 2026-09-01. Falls back to just the plain date on either
 * string if the day-math inputs don't parse cleanly, rather than showing
 * something obviously wrong.
 */
static void format_starts_in(const char *begin_iso, const char *now_iso,
                              char *relative_out, size_t relative_size,
                              char *date_out, size_t date_size)
{
    format_short_date(begin_iso, date_out, date_size);

    int by, bm, bd, ny, nm, nd;
    if (!parse_iso_ymd(begin_iso, &by, &bm, &bd) || !parse_iso_ymd(now_iso, &ny, &nm, &nd)) {
        snprintf(relative_out, relative_size, "Starts %s", date_out);
        return;
    }

    long diff = days_from_civil(by, bm, bd) - days_from_civil(ny, nm, nd);
    if (diff <= 0) {
        snprintf(relative_out, relative_size, "Starts today");
    } else if (diff == 1) {
        snprintf(relative_out, relative_size, "Starts tomorrow");
    } else {
        snprintf(relative_out, relative_size, "Starts in %ld days", diff);
    }
}

static lv_obj_t *watched_roster_container = NULL;
static lv_obj_t *watched_tab_status_lbl = NULL;

static lv_obj_t *make_row_card(lv_obj_t *parent, int y, bool dim_bg, int height = 76)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, 760, height);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, dim_bg ? lv_color_hex(0x0e1320) : COLOR_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COLOR_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    return card;
}

// ---------------------------------------------------------------------------
// Category Activity Feed -- full-screen drill-down (2026-09-06), opened by
// tapping the WATCHED or NEEDED panel on Overview. A back arrow returns to
// Overview; no bottom tab bar while drilled in, per the approved navigation
// convention (this is a new layer on top of the four-tab model, not a fifth
// tab). Built from the approved dxmon_category_feed_mockup.svg. Deliberately
// NOT backed by spot_history.json -- see ActivityData's own struct comment in
// dxmon_client.h for why a category-wide feed doesn't need it.
// ---------------------------------------------------------------------------
static lv_obj_t *activity_feed_screen = NULL;
static lv_obj_t *activity_feed_category_lbl = NULL;
static lv_obj_t *activity_feed_status_lbl = NULL;
static lv_obj_t *activity_feed_container = NULL;

static void activity_back_event_cb(lv_event_t *e)
{
    lv_scr_load(screens[0]);
}

// Added 2026-09-08: each row needs a stable pointer to its own callsign that
// survives until the tap actually fires (rows are rebuilt fresh every
// refresh, so a pointer into a local/temporary string wouldn't survive).
// A static pool sized to MAX_ACTIVITY_SPOTS, refilled each rebuild, does the
// job -- old rows are always destroyed via lv_obj_clean() before new ones
// are built, so stale entries are never actually referenced.
static char activity_row_callsign_pool[MAX_ACTIVITY_SPOTS][16];

static void activity_row_click_cb(lv_event_t *e)
{
    const char *callsign = (const char *)lv_event_get_user_data(e);
    open_history_callsign(callsign);
}

static void make_activity_row(lv_obj_t *container, int index, const ActivitySpot &s, bool is_needed)
{
    int y = index * 84;  // 76px row + 8px gap, matching the roster rows' own convention
    lv_obj_t *card = make_row_card(container, y, false);

    // Added 2026-09-08: tapping a row (an individual real spot) opens that
    // callsign's own Single-Target Spot History. Rows have small badges that
    // are clickable-by-default lv_obj_create() widgets, same as the Overview
    // panels that needed an overlay fix -- but here they only cover a small
    // corner of the row rather than its entire surface, so a direct handler
    // on the row itself is accepted as low-risk (a tap precisely on a badge
    // could fail to register, a normal tap elsewhere on the row will not).
    if (index < MAX_ACTIVITY_SPOTS) {
        strncpy(activity_row_callsign_pool[index], s.callsign, 15);
        activity_row_callsign_pool[index][15] = '\0';
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, activity_row_click_cb, LV_EVENT_CLICKED, activity_row_callsign_pool[index]);
    }

    int detail_y = 12;
    if (is_needed) {
        // ENTITY/SLOT badge + entity name -- only the Needed feed spans
        // multiple entities, so only it needs this row.
        bool is_slot = (strcmp(s.kind, "slot") == 0);
        lv_obj_t *badge = lv_obj_create(card);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 62, 20);
        lv_obj_set_pos(badge, 20, 8);
        lv_obj_set_style_bg_color(badge, is_slot ? COLOR_BADGE_BLUE_BG : COLOR_BADGE_BG, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(badge, is_slot ? COLOR_ACCENT_BLUE : COLOR_ACCENT_AMBER, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_set_style_radius(badge, 5, 0);
        lv_obj_t *badge_lbl = lv_label_create(badge);
        lv_label_set_text(badge_lbl, is_slot ? "SLOT" : "ENTITY");
        lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(badge_lbl, is_slot ? COLOR_BADGE_BLUE_TX : COLOR_ACCENT_AMBER, 0);
        lv_obj_center(badge_lbl);

        make_label(card, s.entity, &lv_font_montserrat_14, COLOR_TEXT_PRIMARY, 94, 10);
        detail_y = 36;
    }

    make_label(card, s.callsign, &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, detail_y);

    // Band/mode uppercased into their own small buffers first -- concatenating
    // then upper-casing the whole detail string would also upper-case "deg"/
    // "mi", which every other beam-heading display in this app deliberately
    // keeps lowercase.
    char band_buf[8];
    strncpy(band_buf, s.band, sizeof(band_buf) - 1);
    band_buf[sizeof(band_buf) - 1] = '\0';
    to_upper_inplace(band_buf);
    char mode_buf[16];
    strncpy(mode_buf, s.mode, sizeof(mode_buf) - 1);
    mode_buf[sizeof(mode_buf) - 1] = '\0';
    to_upper_inplace(mode_buf);

    char detail_buf[48];
    if (s.has_beam) {
        snprintf(detail_buf, sizeof(detail_buf), "%s %s -- %.0f deg / %d mi",
                 band_buf, mode_buf, s.heading_deg, s.distance_mi);
    } else {
        snprintf(detail_buf, sizeof(detail_buf), "%s %s", band_buf, mode_buf);
    }
    make_label(card, detail_buf, &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, detail_y + 24);

    char when_buf[24];
    format_short_datetime(s.received_at, when_buf, sizeof(when_buf));
    lv_obj_t *when_lbl = lv_label_create(card);
    lv_label_set_text(when_lbl, when_buf);
    lv_obj_set_style_text_font(when_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(when_lbl, COLOR_TEXT_MUTED, 0);
    lv_obj_align(when_lbl, LV_ALIGN_TOP_RIGHT, -20, 12);
}

static lv_obj_t *make_screen_activity_feed(void)
{
    lv_obj_t *scr = make_screen();

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 800, 64);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    // Back arrow -- LVGL's own built-in symbol rather than a custom vector
    // shape, avoiding the mockup's hand-drawn circle+chevron for a much
    // simpler, already-proven approach (LV_SYMBOL_WIFI is already used
    // elsewhere in this file the same way).
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 56, 56);
    lv_obj_set_pos(back_btn, 8, 4);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(back_btn, activity_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(back_lbl);

    activity_feed_category_lbl = lv_label_create(header);
    lv_label_set_text(activity_feed_category_lbl, "");
    lv_obj_set_style_text_font(activity_feed_category_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(activity_feed_category_lbl, 64, 10);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "Recent Activity");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_pos(title_lbl, 64, 30);

    activity_feed_status_lbl = lv_label_create(header);
    lv_label_set_text(activity_feed_status_lbl, "LOADING");
    lv_obj_set_style_text_font(activity_feed_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(activity_feed_status_lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_align(activity_feed_status_lbl, LV_ALIGN_TOP_RIGHT, -24, 22);

    make_divider(scr, 0, 64, 800);

    activity_feed_container = lv_obj_create(scr);
    lv_obj_remove_style_all(activity_feed_container);
    lv_obj_set_pos(activity_feed_container, 16, 72);
    lv_obj_set_size(activity_feed_container, 768, 400);
    lv_obj_set_style_bg_opa(activity_feed_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(activity_feed_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(activity_feed_container, LV_SCROLLBAR_MODE_AUTO);

    return scr;
}

static void open_activity_feed(bool is_needed)
{
    lv_label_set_text(activity_feed_category_lbl, is_needed ? "NEEDED" : "WATCHED");
    lv_obj_set_style_text_color(activity_feed_category_lbl, is_needed ? COLOR_ACCENT_AMBER : COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(activity_feed_status_lbl, "LOADING");
    lv_obj_set_style_text_color(activity_feed_status_lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_clean(activity_feed_container);
    lv_scr_load(activity_feed_screen);

    // Fetched on demand when the screen opens, not on a background timer --
    // this is a one-shot drill-down view, not a screen that stays visible
    // for extended periods the way Overview/Watched/Needed do.
    static ActivityData data;
    if (dxmon_fetch_activity(is_needed, data)) {
        lv_label_set_text(activity_feed_status_lbl, "LIVE");
        lv_obj_set_style_text_color(activity_feed_status_lbl, COLOR_STATUS_GREEN, 0);
        if (data.count == 0) {
            lv_obj_t *empty_lbl = lv_label_create(activity_feed_container);
            lv_label_set_text(empty_lbl, "No recent activity yet.");
            lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(empty_lbl, COLOR_TEXT_MUTED, 0);
            lv_obj_set_pos(empty_lbl, 20, 20);
        } else {
            for (int i = 0; i < data.count; i++) {
                make_activity_row(activity_feed_container, i, data.spots[i], is_needed);
            }
        }
    } else {
        lv_label_set_text(activity_feed_status_lbl, "OFFLINE");
        lv_obj_set_style_text_color(activity_feed_status_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_t *err_lbl = lv_label_create(activity_feed_container);
        lv_label_set_text(err_lbl, "Couldn't load activity -- check connection.");
        lv_obj_set_style_text_font(err_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(err_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_pos(err_lbl, 20, 20);
    }
}

// ---------------------------------------------------------------------------
// Single-Target Spot History -- full-screen drill-down (2026-09-08). One
// shared screen, two data-source variants:
//   - Callsign-level: opened from an individual-spot tap (a Category
//     Activity Feed row) or a Watched roster-row tap (a Watched entry
//     already is one fixed callsign). Header shows the callsign plus its
//     entity and beam heading, computed once since every spot shares it.
//   - Entity-level: opened from a Needed roster-row tap. Header shows the
//     entity/slot instead; each row shows its own callsign and beam
//     heading, since a slot can be worked by several different callsigns.
// Full-screen takeover with a back arrow, no bottom tab bar, matching the
// Category Activity Feed's own established convention. Built from the
// approved dxmon_single_target_history_mockup.svg.
// ---------------------------------------------------------------------------
static lv_obj_t *history_screen = NULL;
static lv_obj_t *history_category_lbl = NULL;
static lv_obj_t *history_title_lbl = NULL;
static lv_obj_t *history_subtitle_lbl = NULL;
static lv_obj_t *history_beam_lbl = NULL;
static lv_obj_t *history_status_lbl = NULL;
static lv_obj_t *history_container = NULL;
static lv_obj_t *history_footer_lbl = NULL;

static void history_back_event_cb(lv_event_t *e)
{
    lv_scr_load(screens[0]);
}

static void make_history_row(lv_obj_t *container, int index, const HistorySpot &s, bool show_callsign, const char *now_iso)
{
    // Entity-level rows need more height to fit the per-row callsign+beam
    // line (band/mode can vary per hit); callsign-level rows don't need it,
    // since the callsign and its beam are already shown once in the header.
    int row_height = show_callsign ? 72 : 46;
    int y = index * (row_height + 6);
    lv_obj_t *card = make_row_card(container, y, index % 2 == 1, row_height);

    // Added 2026-09-08: Dan's own real operational point -- a spot more than
    // a few hours old has almost certainly moved bands/modes or gone quiet,
    // so it shouldn't visually compete with a genuinely fresh one. Dims this
    // row's text rather than hiding it -- a sparse Watched callsign's only
    // available history might be old, and that's still worth showing, just
    // not worth emphasizing the same as something actionable right now.
    bool stale = spot_is_stale(s.received_at, now_iso);

    char mode_buf[16];
    strncpy(mode_buf, s.mode, sizeof(mode_buf) - 1);
    mode_buf[sizeof(mode_buf) - 1] = '\0';
    to_upper_inplace(mode_buf);

    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 52, 24);
    lv_obj_set_pos(badge, 20, 11);
    lv_obj_set_style_bg_color(badge, COLOR_BADGE_BLUE_BG, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, 5, 0);
    lv_obj_t *badge_lbl = lv_label_create(badge);
    lv_label_set_text(badge_lbl, mode_buf);
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(badge_lbl, stale_aware_color(stale, COLOR_BADGE_BLUE_TX), 0);
    lv_obj_center(badge_lbl);

    char freq_buf[24];
    snprintf(freq_buf, sizeof(freq_buf), "%s MHz", s.frequency);
    make_label(card, freq_buf, &lv_font_montserrat_14, stale_aware_color(stale, COLOR_TEXT_PRIMARY), 90, 11);

    char band_buf[8];
    strncpy(band_buf, s.band, sizeof(band_buf) - 1);
    band_buf[sizeof(band_buf) - 1] = '\0';
    to_upper_inplace(band_buf);
    make_label(card, band_buf, &lv_font_montserrat_12, COLOR_TEXT_MUTED, 280, 14);

    char when_buf[24];
    format_short_datetime(s.received_at, when_buf, sizeof(when_buf));
    lv_obj_t *when_lbl = lv_label_create(card);
    lv_label_set_text(when_lbl, when_buf);
    lv_obj_set_style_text_font(when_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(when_lbl, stale_aware_color(stale, COLOR_TEXT_PRIMARY), 0);
    lv_obj_align(when_lbl, LV_ALIGN_TOP_RIGHT, -20, 14);

    if (show_callsign) {
        // Entity-level only -- bold/bright callsign matching the styling
        // already established on the Overview panel, with beam inline to
        // its right (measured after layout, same technique used elsewhere).
        // Dims to muted (from its normal bright amber) when stale, same as
        // every other element on this row.
        lv_color_t cs_color = stale_aware_color(stale, COLOR_ACCENT_AMBER);
        lv_obj_t *cs_shadow = lv_label_create(card);
        lv_label_set_text(cs_shadow, s.callsign);
        lv_obj_set_style_text_font(cs_shadow, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(cs_shadow, cs_color, 0);
        lv_obj_set_pos(cs_shadow, 21, 41);

        lv_obj_t *cs_main = lv_label_create(card);
        lv_label_set_text(cs_main, s.callsign);
        lv_obj_set_style_text_font(cs_main, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(cs_main, cs_color, 0);
        lv_obj_set_pos(cs_main, 20, 41);

        char beam_buf[32];
        format_beam_suffix_raw(s.has_beam, s.heading_deg, s.distance_mi, beam_buf, sizeof(beam_buf));
        if (beam_buf[0] != '\0') {
            lv_obj_t *beam_lbl = lv_label_create(card);
            lv_label_set_text(beam_lbl, beam_buf);
            lv_obj_set_style_text_font(beam_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(beam_lbl, COLOR_TEXT_MUTED, 0);
            lv_obj_update_layout(cs_main);
            lv_obj_align_to(beam_lbl, cs_main, LV_ALIGN_OUT_RIGHT_MID, 10, 2);
        }
    }
}

static lv_obj_t *make_screen_single_target_history(void)
{
    lv_obj_t *scr = make_screen();

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 800, 86);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER_BG, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 56, 56);
    lv_obj_set_pos(back_btn, 8, 4);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(back_btn, history_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(back_lbl);

    history_category_lbl = lv_label_create(header);
    lv_label_set_text(history_category_lbl, "");
    lv_obj_set_style_text_font(history_category_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(history_category_lbl, COLOR_ACCENT_BLUE, 0);
    lv_obj_set_pos(history_category_lbl, 64, 8);

    history_title_lbl = lv_label_create(header);
    lv_label_set_text(history_title_lbl, "--");
    lv_obj_set_style_text_font(history_title_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(history_title_lbl, COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_pos(history_title_lbl, 64, 26);

    history_subtitle_lbl = lv_label_create(header);
    lv_label_set_text(history_subtitle_lbl, "");
    lv_obj_set_style_text_font(history_subtitle_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(history_subtitle_lbl, COLOR_TEXT_MUTED, 0);
    lv_label_set_long_mode(history_subtitle_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(history_subtitle_lbl, 520);
    lv_obj_set_pos(history_subtitle_lbl, 64, 60);

    history_beam_lbl = lv_label_create(header);
    lv_label_set_text(history_beam_lbl, "");
    lv_obj_set_style_text_font(history_beam_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(history_beam_lbl, COLOR_ACCENT_BLUE, 0);
    lv_obj_align(history_beam_lbl, LV_ALIGN_TOP_RIGHT, -24, 60);

    history_status_lbl = lv_label_create(header);
    lv_label_set_text(history_status_lbl, "LOADING");
    lv_obj_set_style_text_font(history_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(history_status_lbl, COLOR_BADGE_TEXT, 0);
    lv_obj_align(history_status_lbl, LV_ALIGN_TOP_RIGHT, -24, 8);

    make_divider(scr, 0, 86, 800);

    history_container = lv_obj_create(scr);
    lv_obj_remove_style_all(history_container);
    lv_obj_set_pos(history_container, 16, 94);
    lv_obj_set_size(history_container, 768, 346);
    lv_obj_set_style_bg_opa(history_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(history_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(history_container, LV_SCROLLBAR_MODE_AUTO);

    // Honest data-limit footer, matching the approved mockup -- shown only
    // when fewer than MAX_HISTORY_SPOTS spots are actually available.
    history_footer_lbl = lv_label_create(scr);
    lv_label_set_text(history_footer_lbl, "");
    lv_obj_set_style_text_font(history_footer_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(history_footer_lbl, COLOR_TEXT_MUTED, 0);
    // Real bug found and fixed 2026-09-08: no width was ever set on this
    // label, so it fell back to LVGL's own default fixed object width and
    // clipped the real footer text mid-word ("...not availab") instead of
    // sizing to fit it -- confirmed on real hardware. Width set generously
    // wide (the longest real footer text, "Showing 9 of last 10 -- older
    // spots not available", is well under 400px at this font size) with
    // wrap as a safety net rather than another silent clip if this text is
    // ever lengthened later.
    lv_obj_set_width(history_footer_lbl, 600);
    lv_label_set_long_mode(history_footer_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(history_footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(history_footer_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    return scr;
}

static void render_history_data(const HistoryData &data, bool is_needed)
{
    lv_obj_clean(history_container);
    if (data.count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(history_container);
        lv_label_set_text(empty_lbl, "No real spots recorded yet for this target.");
        lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_pos(empty_lbl, 20, 20);
    } else {
        for (int i = 0; i < data.count; i++) {
            make_history_row(history_container, i, data.spots[i], is_needed, data.updated);
        }
    }

    if (data.count > 0 && data.count < MAX_HISTORY_SPOTS) {
        // Real bug found and fixed 2026-09-08: this buffer was 48 bytes, but
        // the actual message ("Showing 4 of last 10 -- older spots not
        // available") is 49 characters + a null terminator = 50 bytes --
        // snprintf correctly truncated it to fit, cutting the real text at
        // exactly "...not availab", matching what showed up on real
        // hardware precisely. This was never an LVGL rendering/width issue
        // (the earlier fix to the label's own width/wrap/alignment was real
        // and worth keeping, but addressed a different, non-existent
        // problem -- the string was already truncated before it ever
        // reached the label). Sized with real headroom this time, not just
        // barely enough for today's exact wording.
        char footer_buf[64];
        snprintf(footer_buf, sizeof(footer_buf), "Showing %d of last %d -- older spots not available",
                 data.count, MAX_HISTORY_SPOTS);
        lv_label_set_text(history_footer_lbl, footer_buf);
    } else {
        lv_label_set_text(history_footer_lbl, "");
    }
}

static void open_history_callsign(const char *callsign)
{
    lv_label_set_text(history_category_lbl, "CALLSIGN -- LAST 10 SPOTS");
    lv_label_set_text(history_title_lbl, callsign);
    lv_label_set_text(history_subtitle_lbl, "");
    lv_label_set_text(history_beam_lbl, "");
    lv_label_set_text(history_status_lbl, "LOADING");
    lv_obj_set_style_text_color(history_status_lbl, COLOR_BADGE_TEXT, 0);
    lv_label_set_text(history_footer_lbl, "");
    lv_obj_clean(history_container);
    lv_scr_load(history_screen);

    static HistoryData data;
    if (dxmon_fetch_history_callsign(callsign, data)) {
        lv_label_set_text(history_status_lbl, "LIVE");
        lv_obj_set_style_text_color(history_status_lbl, COLOR_STATUS_GREEN, 0);
        char beam_buf[32];
        format_beam_suffix_raw(data.has_beam, data.heading_deg, data.distance_mi, beam_buf, sizeof(beam_buf));
        lv_label_set_text(history_beam_lbl, beam_buf);
        render_history_data(data, false);
    } else {
        lv_label_set_text(history_status_lbl, "OFFLINE");
        lv_obj_set_style_text_color(history_status_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_t *err_lbl = lv_label_create(history_container);
        lv_label_set_text(err_lbl, "Couldn't load spot history -- check connection.");
        lv_obj_set_style_text_font(err_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(err_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_set_pos(err_lbl, 20, 20);
    }
}

static void open_history_needed(const char *needed_id)
{
    lv_label_set_text(history_category_lbl, "NEEDED -- SLOT HISTORY");
    lv_label_set_text(history_title_lbl, "--");
    lv_label_set_text(history_subtitle_lbl, "");
    lv_label_set_text(history_beam_lbl, "");
    lv_label_set_text(history_status_lbl, "LOADING");
    lv_obj_set_style_text_color(history_status_lbl, COLOR_BADGE_TEXT, 0);
    lv_label_set_text(history_footer_lbl, "");
    lv_obj_clean(history_container);
    lv_scr_load(history_screen);

    static HistoryData data;
    if (dxmon_fetch_history_needed(needed_id, data)) {
        lv_label_set_text(history_status_lbl, "LIVE");
        lv_obj_set_style_text_color(history_status_lbl, COLOR_STATUS_GREEN, 0);
        lv_label_set_text(history_title_lbl, data.entity);
        char subtitle_buf[64];
        if (data.band[0] != '\0' || data.mode[0] != '\0') {
            snprintf(subtitle_buf, sizeof(subtitle_buf), "%s%s%s",
                     data.band, (data.band[0] != '\0' && data.mode[0] != '\0') ? " " : "", data.mode);
            to_upper_inplace(subtitle_buf);
        } else {
            snprintf(subtitle_buf, sizeof(subtitle_buf), "WHOLE ENTITY -- ANY BAND/MODE");
        }
        lv_label_set_text(history_subtitle_lbl, subtitle_buf);
        // No top-level beam here -- entity-level rows carry their own,
        // since different rows can be different callsigns (see struct comment).
        render_history_data(data, true);
    } else {
        lv_label_set_text(history_status_lbl, "OFFLINE");
        lv_obj_set_style_text_color(history_status_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_t *err_lbl = lv_label_create(history_container);
        lv_label_set_text(err_lbl, "Couldn't load spot history -- check connection or this entry may have been removed.");
        lv_obj_set_style_text_font(err_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(err_lbl, COLOR_TEXT_MUTED, 0);
        lv_label_set_long_mode(err_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(err_lbl, 700);
        lv_obj_set_pos(err_lbl, 20, 20);
    }
}

// Same pool pattern as activity_row_callsign_pool above, sized for
// MAX_WATCHED_ENTRIES.
static char watched_row_callsign_pool[MAX_WATCHED_ENTRIES][16];

static void make_watched_row(lv_obj_t *container, int index, const WatchedEntry &e, const char *now_iso)
{
    int y = index * 84;  // 76px row + 8px gap, matching the approved mockup
    bool active = e.has_last_spot;
    bool upcoming = !active && e.adxo_active == false && e.adxo_end[0] != '\0';
    // Note: adxo_end is always populated when an ADXO link exists (even for inactive/future
    // entries -- the field just holds whichever date the server sent); adxo_active specifically
    // distinguishes "already live" from "not yet." A real entry with no ADXO link at all (e.g.
    // a manually-added Watched callsign) has adxo_end empty -- the fourth, no-precedent-in-mockup
    // fallback state below.
    bool waiting = !active && e.adxo_active == true;
    bool no_adxo = !active && !upcoming && !waiting;

    // Added 2026-09-10: Dan's own earlier observation (a 44-hour-old Needed
    // last-seen entry still rendering in full bright color) applied here too
    // -- a Watched row's spot can be real but old, and should dim the same
    // way Single-Target History and Needed's Overview panel already do.
    // Reuses the existing 4-hour threshold and helpers unchanged; "active"
    // only means "has a real spot at all," not "is it still fresh," so this
    // is a genuinely separate check layered on top of it.
    bool stale = active && spot_is_stale(e.received_at, now_iso);

    lv_obj_t *card = make_row_card(container, y, !active);

    // Added 2026-09-08: tapping a Watched roster row opens that entry's own
    // callsign-level Single-Target Spot History -- a Watched entry already
    // IS one fixed callsign, so this is the natural mapping. Same pool/
    // click-handler pattern as the Activity Feed rows above.
    if (index < MAX_WATCHED_ENTRIES) {
        strncpy(watched_row_callsign_pool[index], e.callsign, 15);
        watched_row_callsign_pool[index][15] = '\0';
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, activity_row_click_cb, LV_EVENT_CLICKED, watched_row_callsign_pool[index]);
    }

    lv_color_t dot_color = active ? COLOR_STATUS_GREEN : COLOR_DOT_GRAY;
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, 20, 33);

    lv_color_t text_primary = stale ? COLOR_TEXT_MUTED : (active ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECOND);
    lv_color_t text_secondary = stale ? COLOR_TEXT_MUTED : (active ? COLOR_TEXT_SECOND : COLOR_TEXT_MUTED);

    lv_obj_t *cs_lbl = make_label(card, e.callsign, &lv_font_montserrat_26, text_primary, 36, 12);
    // Added 2026-09-06: beam heading inline to the callsign's right, same
    // measure-then-align technique as the Overview panel above.
    char roster_beam_buf[32];
    format_beam_suffix(e, roster_beam_buf, sizeof(roster_beam_buf));
    if (roster_beam_buf[0] != '\0') {
        lv_obj_t *beam_lbl = lv_label_create(card);
        lv_label_set_text(beam_lbl, roster_beam_buf);
        lv_obj_set_style_text_font(beam_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(beam_lbl, text_secondary, 0);
        lv_obj_update_layout(cs_lbl);
        lv_obj_align_to(beam_lbl, cs_lbl, LV_ALIGN_OUT_RIGHT_MID, 10, 3);
    }
    make_label(card, e.dxcc, &lv_font_montserrat_14, text_secondary, 36, 44);

    if (active) {
        lv_obj_t *mode_badge = lv_obj_create(card);
        lv_obj_remove_style_all(mode_badge);
        lv_obj_set_size(mode_badge, 56, 24);
        lv_obj_set_pos(mode_badge, 574, 16);
        lv_obj_set_style_bg_color(mode_badge, COLOR_BADGE_BLUE_BG, 0);
        lv_obj_set_style_bg_opa(mode_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mode_badge, COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_border_width(mode_badge, 1, 0);
        lv_obj_set_style_radius(mode_badge, 6, 0);
        char mode_buf[16];
        strncpy(mode_buf, e.mode, sizeof(mode_buf) - 1);
        mode_buf[sizeof(mode_buf) - 1] = '\0';
        to_upper_inplace(mode_buf);
        lv_obj_t *mode_lbl = lv_label_create(mode_badge);
        lv_label_set_text(mode_lbl, mode_buf);
        lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(mode_lbl, stale_aware_color(stale, COLOR_BADGE_BLUE_TX), 0);
        lv_obj_center(mode_lbl);

        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", e.frequency);
        make_label(card, freq_buf, &lv_font_montserrat_16, stale_aware_color(stale, COLOR_TEXT_PRIMARY), 640, 20);

        // Plain reformatted timestamp, not computed elapsed-time ("2m ago") -- deliberately
        // consistent with Overview's own already-proven treatment, not a new deviation.
        char when_buf[24];
        format_short_datetime(e.received_at, when_buf, sizeof(when_buf));
        lv_obj_t *when_lbl = lv_label_create(card);
        lv_label_set_text(when_lbl, when_buf);
        lv_obj_set_style_text_font(when_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(when_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(when_lbl, LV_ALIGN_TOP_RIGHT, -20, 50);
    } else if (upcoming) {
        lv_obj_t *pill = lv_obj_create(card);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, 176, 42);
        lv_obj_set_pos(pill, 564, 17);
        lv_obj_set_style_bg_color(pill, COLOR_BADGE_BG, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 13, 0);

        char relative_buf[24];
        char date_buf[16];
        format_starts_in(e.adxo_end, now_iso, relative_buf, sizeof(relative_buf), date_buf, sizeof(date_buf));
        // Note: e.adxo_end is reused here as the relevant ADXO date for this row -- see the
        // struct comment; for an upcoming (not-yet-active) entry this is effectively its begin
        // date as sent by the server.

        lv_obj_t *rel_lbl = lv_label_create(pill);
        lv_label_set_text(rel_lbl, relative_buf);
        lv_obj_set_style_text_font(rel_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rel_lbl, COLOR_BADGE_TEXT, 0);
        lv_obj_align(rel_lbl, LV_ALIGN_TOP_MID, 0, 5);

        char date_line[24];
        snprintf(date_line, sizeof(date_line), "(%s)", date_buf);
        lv_obj_t *date_lbl = lv_label_create(pill);
        lv_label_set_text(date_lbl, date_line);
        lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(date_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 23);
    } else if (waiting) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "Awaiting first spot");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -20, 0);
    } else {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "No schedule data");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -20, 0);
    }
}

static void update_watched_roster(const WatchedData &data)
{
    if (!watched_roster_container) return;
    lv_obj_clean(watched_roster_container);
    for (int i = 0; i < data.count; i++) {
        make_watched_row(watched_roster_container, i, data.entries[i], data.updated);
    }

    if (watched_tab_status_lbl) {
        char count_buf[24];
        snprintf(count_buf, sizeof(count_buf), "%d TRACKED", data.count);
        lv_label_set_text(watched_tab_status_lbl, count_buf);
    }
}

static lv_obj_t *make_screen_watched(void)
{
    lv_obj_t *scr = make_screen();
    watched_tab_status_lbl = create_header(scr, "WATCHED", "0 TRACKED");

    watched_roster_container = lv_obj_create(scr);
    lv_obj_remove_style_all(watched_roster_container);
    lv_obj_set_pos(watched_roster_container, 16, 70);
    lv_obj_set_size(watched_roster_container, 760, 344);
    lv_obj_set_style_bg_opa(watched_roster_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(watched_roster_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(watched_roster_container, LV_SCROLLBAR_MODE_AUTO);

    return scr;
}

// ---------------------------------------------------------------------------
// Needed roster (tab bar destination) -- full scrollable list over
// /api/dxmon/needed, Dan's unified curated list (renamed/simplified 2026-09-04
// from /api/dxmon/targets). Reuses Watched roster's proven row-card pattern
// (make_row_card, LV_DIR_VER touch-scroll), extended with a kind badge.
// Three real states now (down from the old five): Live, Recently Seen, Never
// Spotted -- no more Awaiting First Spot / Upcoming, since those depended on
// the ADXO cross-reference this list no longer has (ADXO stays Watched-only).
// ---------------------------------------------------------------------------

static lv_obj_t *needed_roster_container = NULL;
static lv_obj_t *needed_tab_status_lbl = NULL;

// Same pool pattern as the two above, sized for MAX_NEEDED_ENTRIES, keyed by
// the entry's own stable id rather than a callsign (see click-wiring comment
// in make_target_row below).
static char needed_row_id_pool[MAX_NEEDED_ENTRIES][16];

static void needed_row_click_cb(lv_event_t *e)
{
    const char *needed_id = (const char *)lv_event_get_user_data(e);
    open_history_needed(needed_id);
}

static void make_target_row(lv_obj_t *container, int index, const NeededEntry &t)
{
    // Row grown 76 -> 96px (2026-09-05) to fit the new callsign+beam line below --
    // Watched's own make_watched_row() is untouched, still 76px, since
    // make_row_card()'s new height parameter defaults to 76 when not passed.
    const int row_height = 96;
    const int row_gap = 8;
    int y = index * (row_height + row_gap);
    bool is_slot = (strcmp(t.kind, "slot") == 0);
    bool live = t.last_spot.present;
    bool seen = !live && t.last_seen.present;
    // else: never spotted -- the common case for a freshly-added entry.

    lv_obj_t *card = make_row_card(container, y, !live, row_height);

    // Added 2026-09-08: tapping a Needed roster row opens that entry's
    // entity-level Single-Target Spot History -- uses the entry's own
    // stable id (not its entity name, which could theoretically collide or
    // change), matching the same key spot_history.json itself uses server-side.
    if (index < MAX_NEEDED_ENTRIES) {
        strncpy(needed_row_id_pool[index], t.id, 15);
        needed_row_id_pool[index][15] = '\0';
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, needed_row_click_cb, LV_EVENT_CLICKED, needed_row_id_pool[index]);
    }

    lv_color_t dot_color = live ? COLOR_STATUS_GREEN : (seen ? COLOR_ACCENT_AMBER : COLOR_DOT_GRAY);
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, 20, 33);

    // Kind badge -- derived from band/mode presence (2026-09-04), replacing the old
    // stored needed/wanted type field. Same amber/blue identity colors carried
    // forward from the pre-merge NEEDED/WANTED badge, just relabeled ENTITY/SLOT.
    lv_obj_t *type_badge = lv_obj_create(card);
    lv_obj_remove_style_all(type_badge);
    lv_obj_set_size(type_badge, 62, 20);
    lv_obj_set_pos(type_badge, 36, 10);
    lv_obj_set_style_radius(type_badge, 5, 0);
    lv_obj_set_style_border_width(type_badge, 1, 0);
    lv_obj_set_style_bg_color(type_badge, is_slot ? COLOR_BADGE_BLUE_BG : lv_color_hex(0x332a10), 0);
    lv_obj_set_style_bg_opa(type_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(type_badge, is_slot ? COLOR_ACCENT_BLUE : COLOR_ACCENT_AMBER, 0);
    lv_obj_t *type_lbl = lv_label_create(type_badge);
    lv_label_set_text(type_lbl, is_slot ? "SLOT" : "ENTITY");
    lv_obj_set_style_text_font(type_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(type_lbl, is_slot ? COLOR_BADGE_BLUE_TX : COLOR_ACCENT_AMBER, 0);
    lv_obj_center(type_lbl);

    lv_color_t text_primary = live ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECOND;
    lv_obj_t *entity_lbl = lv_label_create(card);
    lv_label_set_text(entity_lbl, t.entity);
    lv_obj_set_style_text_font(entity_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(entity_lbl, text_primary, 0);
    lv_label_set_long_mode(entity_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(entity_lbl, 360);
    lv_obj_set_pos(entity_lbl, 36, 34);

    // Subtitle: band/mode for a slot target. Entity-kind targets have no
    // equivalent now that the old CSV "prefix" field is gone -- row omits the
    // subtitle line's text entirely rather than showing something misleading.
    if (is_slot) {
        char sub_buf[32];
        snprintf(sub_buf, sizeof(sub_buf), "%s%s%s", t.band, (t.band[0] && t.mode[0]) ? " " : "", t.mode);
        make_label(card, sub_buf, &lv_font_montserrat_12, COLOR_TEXT_MUTED, 36, 56);
    }

    if (live || seen) {
        const SpotInfo &spot = live ? t.last_spot : t.last_seen;

        lv_obj_t *mode_badge = lv_obj_create(card);
        lv_obj_remove_style_all(mode_badge);
        lv_obj_set_size(mode_badge, 56, 24);
        lv_obj_set_pos(mode_badge, 574, 16);
        lv_obj_set_style_bg_color(mode_badge, COLOR_BADGE_BLUE_BG, 0);
        lv_obj_set_style_bg_opa(mode_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mode_badge, COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_border_width(mode_badge, 1, 0);
        lv_obj_set_style_radius(mode_badge, 6, 0);
        char mode_buf[16];
        strncpy(mode_buf, spot.mode, sizeof(mode_buf) - 1);
        mode_buf[sizeof(mode_buf) - 1] = '\0';
        to_upper_inplace(mode_buf);
        lv_obj_t *mode_lbl = lv_label_create(mode_badge);
        lv_label_set_text(mode_lbl, mode_buf);
        lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(mode_lbl, COLOR_BADGE_BLUE_TX, 0);
        lv_obj_center(mode_lbl);

        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", spot.frequency);
        make_label(card, freq_buf, &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 640, 20);

        char when_buf[24];
        format_short_datetime(spot.received_at, when_buf, sizeof(when_buf));
        lv_obj_t *when_lbl = lv_label_create(card);
        // "Last hit" prefix distinguishes Tier 2 (persisted, possibly old) from a
        // genuinely live Tier 1 row at a glance, without needing a second badge.
        char when_prefixed[32];
        snprintf(when_prefixed, sizeof(when_prefixed), "%s%s", live ? "" : "Last hit: ", when_buf);
        lv_label_set_text(when_lbl, when_prefixed);
        lv_obj_set_style_text_font(when_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(when_lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(when_lbl, LV_ALIGN_TOP_RIGHT, -20, 50);

        // Added 2026-09-05: the spotted callsign + beam heading. Fixed y=76 works
        // for both kinds -- entity-kind rows have no subtitle line at y=56, and
        // slot-kind rows' subtitle text ends well before y=76, so there's no
        // collision either way.
        // 2026-09-06: callsign made more prominent per Dan's request -- larger,
        // amber, real double-draw bold (shadow + main, same pattern as
        // Overview's Tier 1/2) -- with the beam suffix aligned to its right
        // once its real rendered width is known, reusing the same
        // measure-then-position technique as make_status_indicator().
        if (spot.callsign[0] != '\0') {
            lv_obj_t *cs_shadow = lv_label_create(card);
            lv_label_set_text(cs_shadow, spot.callsign);
            lv_obj_set_style_text_font(cs_shadow, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(cs_shadow, COLOR_ACCENT_AMBER, 0);
            lv_obj_set_pos(cs_shadow, 37, 74);

            lv_obj_t *cs_main = lv_label_create(card);
            lv_label_set_text(cs_main, spot.callsign);
            lv_obj_set_style_text_font(cs_main, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(cs_main, COLOR_ACCENT_AMBER, 0);
            lv_obj_set_pos(cs_main, 36, 74);

            if (spot.has_beam) {
                char beam_buf[32];
                format_beam_suffix(spot, beam_buf, sizeof(beam_buf));
                lv_obj_t *beam_lbl = lv_label_create(card);
                lv_label_set_text(beam_lbl, beam_buf);
                lv_obj_set_style_text_font(beam_lbl, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(beam_lbl, COLOR_TEXT_MUTED, 0);
                lv_obj_update_layout(cs_main);
                lv_obj_align_to(beam_lbl, cs_main, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
            }
        }
    } else {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "Never spotted");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT_MUTED, 0);
        lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -20, 0);
    }
}

static void update_needed_roster(const NeededData &data)
{
    if (!needed_roster_container) return;
    lv_obj_clean(needed_roster_container);
    for (int i = 0; i < data.count; i++) {
        make_target_row(needed_roster_container, i, data.entries[i]);
    }

    if (needed_tab_status_lbl) {
        char count_buf[24];
        snprintf(count_buf, sizeof(count_buf), "%d TRACKED", data.count);
        lv_label_set_text(needed_tab_status_lbl, count_buf);
    }
}

static lv_obj_t *make_screen_needed(void)
{
    lv_obj_t *scr = make_screen();
    needed_tab_status_lbl = create_header(scr, "NEEDED", "0 TRACKED");

    needed_roster_container = lv_obj_create(scr);
    lv_obj_remove_style_all(needed_roster_container);
    lv_obj_set_pos(needed_roster_container, 16, 70);
    lv_obj_set_size(needed_roster_container, 760, 344);
    lv_obj_set_style_bg_opa(needed_roster_container, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(needed_roster_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(needed_roster_container, LV_SCROLLBAR_MODE_AUTO);

    return scr;
}

static void do_full_refresh(void)
{
    // Real diagnostic addition, 2026-09-04 -- added after doubling the RGB bounce buffer
    // (main.cpp, setup()) made both the display glitch AND Wi-Fi connect failure WORSE on
    // one real test, then reverting it fixed both. That result points at internal DRAM
    // headroom as the real tight constraint, not bounce buffer size directly -- but it's
    // still a hypothesis, not confirmed. Logging free internal (DMA-capable) heap and free
    // PSRAM once per refresh cycle (~60s, or on Force Refresh) is cheap and gives real
    // numbers to correlate against the next glitch occurrence, rather than guessing at
    // further display-config changes. Watch specifically for a real dip in the internal
    // heap number around/before an occurrence.
    Serial.printf("Heap free -- internal: %u bytes, PSRAM: %u bytes\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // `static`, not stack-local, deliberately -- the real 2026-09-02 bug this avoids
    // was a stack overflow from a large struct declared as a local variable here
    // (the ESP32's default task stack is only ~8KB), confirmed via real hardware
    // showing corrupted, flickering screen content -- a classic stack-overflow
    // symptom, not a mysterious glitch. WatchedData itself is small (max 10
    // entries); the much larger Needed data below is PSRAM-heap-allocated instead
    // of static, for reasons specific to its own size history -- see its own
    // comment further down.
    static WatchedData data;
    if (dxmon_fetch_watched(data)) {
        Serial.printf("Watched refresh OK, %d entr%s\n", data.count, data.count == 1 ? "y" : "ies");
        lvgl_port_lock(-1);
        update_overview_watched(data);
        update_watched_roster(data);
        lvgl_port_unlock();
    } else {
        Serial.println("Watched refresh failed -- keeping last known-good data");
    }

    // PSRAM-backed, not `static` in internal DRAM -- kept from the original Targets-
    // era fix (2026-09-03: combining a large struct as internal-DRAM `static` with
    // the LVGL pool increase in lv_conf.h overflowed the ESP32-S3's internal SRAM
    // budget by ~35KB at link time). NeededData is much smaller now at
    // MAX_NEEDED_ENTRIES=30 (2026-09-04's unified curated-list redesign) -- no longer
    // close to that budget conflict -- but PSRAM allocation stays as sound general
    // practice rather than being moved back to internal RAM for no real benefit.
    // Only touched once per ~60s fetch, so PSRAM's slightly slower access is
    // irrelevant here -- unlike, say, a display frame buffer touched every frame.
    // Allocated once, lazily, on first use (not at global/static-init time, since
    // PSRAM isn't ready that early).
    static NeededData *needed = nullptr;
    if (!needed) {
        needed = (NeededData *)heap_caps_malloc(sizeof(NeededData), MALLOC_CAP_SPIRAM);
        if (!needed) {
            Serial.println("do_full_refresh: PSRAM allocation for NeededData failed");
        }
    }
    if (needed && dxmon_fetch_needed(*needed)) {
        Serial.printf("Needed refresh OK, %d entr%s\n", needed->count, needed->count == 1 ? "y" : "ies");
        lvgl_port_lock(-1);
        update_overview_needed(*needed);
        update_needed_roster(*needed);
        lvgl_port_unlock();
    } else {
        Serial.println("Needed refresh failed -- keeping last known-good data");
    }

    PreviewStatus ps;
    if (dxmon_fetch_preview_status(ps)) {
        Serial.println("Preview status refresh OK");
        lvgl_port_lock(-1);
        update_config_preview_status(ps);
        lvgl_port_unlock();
    } else {
        Serial.println("Preview status refresh failed -- keeping last known-good data");
    }
}

void setup()
{
    Serial.begin(115200);

    Serial.println("Initializing board");
    Board *board = new Board();
    board->init();

#if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        // REVERTED to 10 rows, 2026-09-04 -- doubling to 20 (tried same day) made things
        // WORSE on the one real test: the visual glitch got more severe (a much larger
        // portion of the frame shifted, not just the tab bar) AND Wi-Fi failed to connect
        // within WIFI_CONNECT_TIMEOUT_MS for the first time ever seen. Real, plausible
        // mechanism: the extra ~16KB the larger bounce buffer claims from internal DRAM
        // cut into the already-tight margin freed up during the 2026-09-03 memory arc
        // (TargetsData/NeededData moved to PSRAM specifically to make room for the LVGL
        // pool increase) -- Wi-Fi's own connection setup also needs internal DMA-capable
        // memory. Reverted to the known-working value rather than stacking another change
        // on an unconfirmed negative result. Do not re-attempt a larger bounce buffer
        // without first freeing more internal DRAM elsewhere, or confirming via Serial
        // heap-free logging that the increase isn't the real cause.
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    Serial.println("Building DXMon UI");
    lvgl_port_lock(-1);

    screens[0] = make_screen_overview();
    screens[1] = make_screen_watched();
    screens[2] = make_screen_needed();
    screens[3] = make_screen_config();
    activity_feed_screen = make_screen_activity_feed();
    history_screen = make_screen_single_target_history();

    for (int i = 0; i < 4; i++) {
        create_tab_bar(screens[i], i);
    }

    lv_scr_load(screens[0]);
    update_wifi_glyph();
    update_config_wifi();

    lvgl_port_unlock();

    Serial.println("Connecting Wi-Fi");
    bool wifi_ok = wifi_connect(WIFI_CONNECT_TIMEOUT_MS);

    lvgl_port_lock(-1);
    update_wifi_glyph();
    update_config_wifi();
    lvgl_port_unlock();

    if (wifi_ok) {
        do_full_refresh();
    } else {
        lvgl_port_lock(-1);
        lv_label_set_text(ov.watched_status_lbl, "NO WI-FI");
        lv_label_set_text(ov.watched_dxcc, "Wi-Fi connection failed");
        reposition_status_indicator(ov.watched_status_lbl, ov.watched_status_dot);
        lvgl_port_unlock();
    }

    last_fetch_ms = millis();
}

void loop()
{
    // Scheduled-reboot mitigation for the unresolved display-rendering glitch -- see
    // config.h's SCHEDULED_REBOOT_INTERVAL_MS comment for the full reasoning. Checked
    // first, before any refresh/redraw work, so it never fires mid-fetch or mid-render.
    // Logged clearly as a deliberate restart so it's never mistaken for a crash when
    // reviewing Serial output later.
    if (millis() >= SCHEDULED_REBOOT_INTERVAL_MS) {
        Serial.printf("Scheduled reboot -- uptime %lu ms reached %lu ms interval, restarting\n",
                      (unsigned long)millis(), (unsigned long)SCHEDULED_REBOOT_INTERVAL_MS);
        delay(100);  // let the Serial line actually flush before the restart cuts power to the peripheral
        ESP.restart();
    }

    bool time_for_refresh = (WiFi.status() == WL_CONNECTED && millis() - last_fetch_ms >= LIVE_FETCH_INTERVAL_MS);

    if (force_refresh_requested || time_for_refresh) {
        force_refresh_requested = false;
        if (WiFi.status() == WL_CONNECTED) {
            do_full_refresh();
        } else {
            Serial.println("Refresh requested but Wi-Fi not connected");
        }
        last_fetch_ms = millis();
    }

    lvgl_port_lock(-1);
    update_wifi_glyph();
    update_config_wifi();
    advance_ticker_if_needed();
    update_flash_states();
    lvgl_port_unlock();

    delay(200);
}
