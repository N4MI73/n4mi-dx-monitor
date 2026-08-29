/**
 * N4MI DXMon -- real firmware UI with live data.
 *
 * Build order (2026-08-29 session):
 *   1. Four-tab navigation shell -- confirmed working on real hardware.
 *   2. Overview's real content against mock data -- confirmed working,
 *      including a real clipping-bug fix (ACTIVE status indicator).
 *   3. This step: real Wi-Fi + live fetch from /api/dxmon/watched,
 *      replacing Overview's mock WATCHED content with real data, and
 *      switching the NEEDED panel to an honest placeholder (no backend
 *      exists for it yet -- showing static mock DX data next to genuinely
 *      live data would be misleading).
 *
 * Watched/Needed/Config tabs remain honest "Not yet built" placeholders.
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
// Small date/time formatting helpers -- deliberately lightweight.
// Real "elapsed time" math (NTP sync + full ISO parsing + day-count
// arithmetic) is scoped OUT of this pass -- see session notes. These just
// reformat the raw ISO strings the server already sends into something more
// readable, with no relative-time computation.
// ---------------------------------------------------------------------------
static const char *MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// "2026-08-29" or "2026-08-29T11:10:00..." -> "Aug 29"
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

// "2026-08-25T11:10:00.127182-04:00" -> "Aug 25, 11:10"
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
// ---------------------------------------------------------------------------
static void create_header(lv_obj_t *parent, const char *title, const char *status)
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
 * Right-aligned status dot + label. Right-aligns the label first, then
 * measures its real rendered width via lv_obj_update_layout() before placing
 * the dot -- fixes a real clipping bug found on real hardware 2026-08-29,
 * where a fixed pixel x position for the label didn't leave enough margin
 * for "ACTIVE"'s actual text width before the panel's right edge clipped it.
 * Returns the label object so callers can update its text/color live.
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

/**
 * Re-runs make_status_indicator's positioning math against an existing
 * label whose text just changed -- lets the dot stay correctly placed next
 * to text of a different length on a live update, not just at creation.
 */
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
// placeholder (no backend exists for it yet). Widget pointers are kept so
// a later fetch can update WATCHED's content in place, without tearing down
// and rebuilding the whole screen.
// ---------------------------------------------------------------------------
struct OverviewWidgets {
    lv_obj_t *watched_status_lbl;
    lv_obj_t *watched_status_dot;
    lv_obj_t *watched_callsign;
    lv_obj_t *watched_dxcc;
    lv_obj_t *watched_freq;
    lv_obj_t *watched_mode_badge;
    lv_obj_t *watched_mode_lbl;
    lv_obj_t *watched_last_spot;
    lv_obj_t *watched_active_through;
    lv_obj_t *watched_badge;
    lv_obj_t *watched_badge_lbl;
};
static OverviewWidgets ov;

static lv_obj_t *make_screen_overview(void)
{
    lv_obj_t *scr = make_screen();
    create_header(scr, "DXMON", "LIVE");

    // --- WATCHED panel (left) -- built with honest "not yet fetched" state ---
    lv_obj_t *watched = make_panel(scr, 16, 72, 378, 316);

    make_label(watched, "WATCHED", &lv_font_montserrat_16, COLOR_ACCENT_BLUE, 20, 24);
    ov.watched_status_lbl = make_status_indicator(watched, &ov.watched_status_dot, COLOR_DOT_GRAY,
                                                   "CONNECTING", COLOR_TEXT_MUTED, 20, 24);

    ov.watched_callsign = make_label(watched, "--", &lv_font_montserrat_30, COLOR_TEXT_PRIMARY, 20, 58);
    ov.watched_dxcc = make_label(watched, "Waiting for first fetch...", &lv_font_montserrat_16,
                                  COLOR_TEXT_SECOND, 20, 100);

    make_divider(watched, 20, 134, 338);

    make_label(watched, "FREQUENCY", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 152);
    ov.watched_freq = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 20, 172);

    make_label(watched, "MODE", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 164, 152);
    ov.watched_mode_badge = lv_obj_create(watched);
    lv_obj_remove_style_all(ov.watched_mode_badge);
    lv_obj_set_size(ov.watched_mode_badge, 56, 24);
    lv_obj_set_pos(ov.watched_mode_badge, 164, 172);
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

    make_label(watched, "LAST SPOT", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 254, 152);
    ov.watched_last_spot = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_PRIMARY, 254, 172);

    make_divider(watched, 20, 212, 338);

    make_label(watched, "ACTIVE THROUGH", &lv_font_montserrat_12, COLOR_TEXT_MUTED, 20, 230);
    ov.watched_active_through = make_label(watched, "--", &lv_font_montserrat_16, COLOR_TEXT_SECOND, 20, 252);

    ov.watched_badge = make_pill_badge(watched, "", 20, 280);
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

/**
 * Selects which watched entry to feature: prefers an entry with a real
 * last_spot (genuinely on the air right now), tie-broken by most recent
 * received_at -- ISO 8601 timestamps sort correctly as plain strings, so
 * strcmp() alone is enough, no date parsing needed for this comparison.
 * Falls back to the first entry if none have been spotted yet.
 */
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
    } else {
        lv_label_set_text(ov.watched_freq, "--");
        lv_label_set_text(ov.watched_mode_lbl, "--");
        lv_label_set_text(ov.watched_last_spot, "Not yet spotted");
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

// ---------------------------------------------------------------------------
// Honest placeholders -- Watched/Needed/Config tabs not yet built.
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
    screens[1] = make_screen_placeholder("WATCHED", "0 TRACKED");
    screens[2] = make_screen_placeholder("NEEDED", "0 REMAINING");
    screens[3] = make_screen_placeholder("CONFIG", "v0.1-dev");

    for (int i = 0; i < 4; i++) {
        create_tab_bar(screens[i], i);
    }

    lv_scr_load(screens[0]);

    lvgl_port_unlock();

    Serial.println("Connecting Wi-Fi");
    bool wifi_ok = wifi_connect(WIFI_CONNECT_TIMEOUT_MS);

    if (wifi_ok) {
        WatchedData data;
        if (dxmon_fetch_watched(data)) {
            Serial.printf("Initial fetch OK, %d watched entr%s\n", data.count, data.count == 1 ? "y" : "ies");
            lvgl_port_lock(-1);
            update_overview_watched(data);
            lvgl_port_unlock();
        } else {
            Serial.println("Initial fetch failed -- leaving 'connecting' placeholder state");
        }
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
    if (WiFi.status() == WL_CONNECTED && millis() - last_fetch_ms >= LIVE_FETCH_INTERVAL_MS) {
        WatchedData data;
        if (dxmon_fetch_watched(data)) {
            Serial.printf("Refresh OK, %d watched entr%s\n", data.count, data.count == 1 ? "y" : "ies");
            lvgl_port_lock(-1);
            update_overview_watched(data);
            lvgl_port_unlock();
        } else {
            Serial.println("Refresh fetch failed -- keeping last known-good data on screen");
        }
        last_fetch_ms = millis();
    }
    delay(200);
}
