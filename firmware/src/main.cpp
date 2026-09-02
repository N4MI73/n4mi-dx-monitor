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
#include <esp_display_panel.hpp>

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
    lv_obj_t *watched_status_lbl;
    lv_obj_t *watched_status_dot;
    lv_obj_t *watched_callsign;
    lv_obj_t *watched_dxcc;
    lv_obj_t *watched_freq;
    lv_obj_t *watched_mode_badge;
    lv_obj_t *watched_mode_lbl;
    lv_obj_t *watched_last_spot;
    lv_obj_t *watched_comment;
    lv_obj_t *watched_active_through;
    lv_obj_t *watched_badge;
    lv_obj_t *watched_badge_lbl;
};
static OverviewWidgets ov;

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

    make_label(watched, "WATCHED", &lv_font_montserrat_16, COLOR_ACCENT_BLUE, 20, 18);
    ov.watched_status_lbl = make_status_indicator(watched, &ov.watched_status_dot, COLOR_DOT_GRAY,
                                                   "CONNECTING", COLOR_TEXT_MUTED, 20, 18);

    ov.watched_callsign = make_label(watched, "--", &lv_font_montserrat_30, COLOR_TEXT_PRIMARY, 20, 44);
    ov.watched_dxcc = make_label(watched, "Waiting for first fetch...", &lv_font_montserrat_16,
                                  COLOR_TEXT_SECOND, 20, 84);

    make_divider(watched, 20, 112, 338);

    make_label(watched, "FREQUENCY", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 124);
    ov.watched_freq = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, 142);

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

    // --- NEEDED panel (right) -- honest placeholder, no backend exists yet ---
    lv_obj_t *needed = make_panel(scr, 406, 72, 378, 316);
    make_label(needed, "NEEDED", &lv_font_montserrat_16, COLOR_ACCENT_AMBER, 20, 24);
    lv_obj_t *needed_msg = lv_label_create(needed);
    lv_label_set_text(needed_msg, "Not yet built");
    lv_obj_set_style_text_font(needed_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(needed_msg, COLOR_TEXT_MUTED, 0);
    lv_obj_center(needed_msg);

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
        lv_label_set_text(ov.watched_dxcc, "Watchlist is empty");
        lv_label_set_text(ov.watched_freq, "--");
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

    if (e.has_last_spot) {
        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", e.frequency);
        lv_label_set_text(ov.watched_freq, freq_buf);

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
        lv_label_set_text(ov.watched_mode_lbl, "--");
        lv_label_set_text(ov.watched_last_spot, "Not yet spotted");
        lv_label_set_text(ov.watched_comment, "");
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
    create_header(scr, "CONFIG", "v0.1-dev");

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

    make_config_row(panel, nullptr, "Firmware", "v0.1-dev", COLOR_TEXT_PRIMARY, 227);

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

static lv_obj_t *make_row_card(lv_obj_t *parent, int y, bool dim_bg)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, 760, 76);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, dim_bg ? lv_color_hex(0x0e1320) : COLOR_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COLOR_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    return card;
}

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

    lv_obj_t *card = make_row_card(container, y, !active);

    lv_color_t dot_color = active ? COLOR_STATUS_GREEN : COLOR_DOT_GRAY;
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot, 20, 33);

    lv_color_t text_primary = active ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECOND;
    lv_color_t text_secondary = active ? COLOR_TEXT_SECOND : COLOR_TEXT_MUTED;

    make_label(card, e.callsign, &lv_font_montserrat_26, text_primary, 36, 12);
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
        lv_obj_set_style_text_color(mode_lbl, COLOR_BADGE_BLUE_TX, 0);
        lv_obj_center(mode_lbl);

        char freq_buf[24];
        snprintf(freq_buf, sizeof(freq_buf), "%s MHz", e.frequency);
        make_label(card, freq_buf, &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 640, 20);

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

static void do_full_refresh(void)
{
    WatchedData data;
    if (dxmon_fetch_watched(data)) {
        Serial.printf("Watched refresh OK, %d entr%s\n", data.count, data.count == 1 ? "y" : "ies");
        lvgl_port_lock(-1);
        update_overview_watched(data);
        update_watched_roster(data);
        lvgl_port_unlock();
    } else {
        Serial.println("Watched refresh failed -- keeping last known-good data");
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
    screens[2] = make_screen_placeholder("NEEDED", "0 REMAINING");
    screens[3] = make_screen_config();

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
    lvgl_port_unlock();

    delay(200);
}
