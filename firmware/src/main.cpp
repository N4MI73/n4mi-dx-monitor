/**
 * N4MI DXMon -- PlatformIO port, first-build reference parity target.
 *
 * This is intentionally near-identical to the Arduino IDE reference sketch
 * (09_lvgl_Porting.ino) confirmed working on real hardware 2026-08-27.
 * Goal for the FIRST PlatformIO build: reproduce that exact same result
 * (Hello World + LVGL Widgets Demo, touch-responsive) under the new
 * toolchain, before any DXMon-specific screen code is added.
 *
 * If this doesn't match the Arduino IDE reference on first flash, the
 * difference is isolated to the PlatformIO toolchain/config -- not a new
 * hardware or library-version unknown, since every version here is pinned
 * to exactly what the Arduino IDE reference used.
 */

#include <Arduino.h>
#include <esp_display_panel.hpp>

#include <lvgl.h>
#include "lvgl_v8_port.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/**
 * lv_demo_widgets() (used by the Arduino IDE reference sketch) lives in LVGL's demos/ folder,
 * which is intentionally excluded from the packaged library -- LVGL's own docs say it must be
 * manually added to a project, it's not part of the buildable library tree at all. No PlatformIO
 * LDF mode discovers it because there's genuinely nothing there to discover.
 *
 * Swapped in a small hand-built touch test instead: a button using core LVGL widgets (already
 * proven compiling in this exact toolchain) that changes color and label text on tap. This
 * validates touch + rendering under PlatformIO at least as directly as the demo would have --
 * arguably more so, since it's a real interaction rather than a static screen.
 */
static void btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);

    static bool toggled = false;
    toggled = !toggled;

    if (toggled) {
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_label_set_text(label, "Touch OK!");
    } else {
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_label_set_text(label, "Tap the button");
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
    // When avoid tearing function is enabled, the frame buffer number should be set in the board driver
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    /**
     * As the anti-tearing feature typically consumes more PSRAM bandwidth, for the ESP32-S3, we
     * need to utilize the "bounce buffer" functionality to enhance the RGB data bandwidth.
     * This feature will consume `bounce_buffer_size * bytes_per_pixel * 2` of SRAM memory.
     */
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    Serial.println("Creating UI");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    lvgl_port_lock(-1);

    /**
     * Create the simple labels -- same reference-parity content as the Arduino IDE sketch,
     * including the on-screen LVGL/ESP32_Display_Panel version readout that confirmed the
     * real versions running during the 8/27 bring-up.
     */
    lv_obj_t *label_1 = lv_label_create(lv_scr_act());
    lv_label_set_text(label_1, "Hello World!");
    lv_obj_set_style_text_font(label_1, &lv_font_montserrat_30, 0);
    lv_obj_align(label_1, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *label_2 = lv_label_create(lv_scr_act());
    lv_label_set_text_fmt(
        label_2, "ESP32_Display_Panel (%d.%d.%d)",
        ESP_PANEL_VERSION_MAJOR, ESP_PANEL_VERSION_MINOR, ESP_PANEL_VERSION_PATCH
    );
    lv_obj_set_style_text_font(label_2, &lv_font_montserrat_16, 0);
    lv_obj_align_to(label_2, label_1, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_obj_t *label_3 = lv_label_create(lv_scr_act());
    lv_label_set_text_fmt(label_3, "LVGL (%d.%d.%d)", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_obj_set_style_text_font(label_3, &lv_font_montserrat_16, 0);
    lv_obj_align_to(label_3, label_2, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /**
     * Touch test -- see the note above lv_demo_widgets' replacement. A real, tappable button
     * confirms both rendering and touch input under the new toolchain.
     */
    lv_obj_t *status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(status_label, "Tap the button");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_align_to(status_label, label_3, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 160, 60);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_align_to(btn, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, status_label);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Tap Me");
    lv_obj_center(btn_label);

    /* Release the mutex */
    lvgl_port_unlock();
}

void loop()
{
    Serial.println("IDLE loop");
    delay(1000);
}
