#include "dxmon_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

/**
 * Real bug found and fixed 2026-09-03: JsonDocument's DEFAULT allocator uses plain
 * malloc(), which draws from the ESP32-S3's limited internal heap -- NOT the board's
 * 8MB of PSRAM, contrary to an earlier (wrong) assumption in this file. A large,
 * deeply-nested response (the real Targets feed, 66+ entries) needs meaningfully more
 * memory to parse than its raw JSON size, and was exhausting internal heap and
 * crashing the device -- confirmed via a real hardware video showing a crash/reboot
 * cycle, with the Targets fetch never succeeding even across multiple refresh
 * attempts. Exact pattern from ArduinoJson's own v7 documentation
 * (arduinojson.org/v7/how-to/use-external-ram-on-esp32/), applied only where the
 * response is genuinely large (Targets) -- Watched's own small response (max 10
 * entries) doesn't need this and keeps using the default allocator.
 */
struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void *pointer) override {
        heap_caps_free(pointer);
    }
    void *reallocate(void *ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};

static void copy_field(char *dest, size_t dest_size, JsonVariant v)
{
    if (v.isNull()) {
        dest[0] = '\0';
        return;
    }
    const char *s = v.as<const char *>();
    if (!s) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, s, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

bool dxmon_fetch_preview_status(PreviewStatus &out)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("dxmon_fetch_preview_status: Wi-Fi not connected");
        return false;
    }

    HTTPClient http;
    String url = String("http://") + DXMON_SERVER_HOST + ":" + DXMON_SERVER_PORT + DXMON_PREVIEW_STATUS_PATH;
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("dxmon_fetch_preview_status: HTTP GET failed, code %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("dxmon_fetch_preview_status: JSON parse failed: %s\n", err.c_str());
        return false;
    }

    PreviewStatus temp;
    temp.adxo_entry_count = doc["adxo"]["entry_count"] | 0;
    copy_field(temp.adxo_updated, sizeof(temp.adxo_updated), doc["adxo"]["updated"]);
    temp.hamalert_connected = doc["hamalert"]["connected"] | false;
    temp.hamalert_enabled = doc["hamalert"]["enabled"] | false;
    temp.hamalert_logged_in = doc["hamalert"]["logged_in"] | false;
    temp.watched_count = doc["watched_count"] | 0;

    out = temp;
    return true;
}

static void parse_spot_info(SpotInfo &spot, JsonVariant v)
{
    if (v.isNull()) {
        spot.present = false;
        return;
    }
    spot.present = true;
    copy_field(spot.callsign, sizeof(spot.callsign), v["callsign"]);
    copy_field(spot.band, sizeof(spot.band), v["band"]);
    copy_field(spot.mode, sizeof(spot.mode), v["mode"]);
    copy_field(spot.frequency, sizeof(spot.frequency), v["frequency"]);
    copy_field(spot.received_at, sizeof(spot.received_at), v["received_at"]);
    copy_field(spot.comment, sizeof(spot.comment), v["comment"]);
}

bool dxmon_fetch_targets(TargetsData &out)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("dxmon_fetch_targets: Wi-Fi not connected");
        return false;
    }

    HTTPClient http;
    String url = String("http://") + DXMON_SERVER_HOST + ":" + DXMON_SERVER_PORT + DXMON_TARGETS_PATH;
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("dxmon_fetch_targets: HTTP GET failed, code %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Uses PSRAM explicitly (see SpiRamAllocator above) -- this is the one parse in
    // the codebase large enough for it to matter. Constructed here, locally, not at
    // file scope -- PSRAM isn't ready to use before setup() runs, so a global
    // allocator/document would crash at static-init time.
    SpiRamAllocator allocator;
    JsonDocument doc(&allocator);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("dxmon_fetch_targets: JSON parse failed: %s\n", err.c_str());
        return false;
    }

    JsonArray arr = doc["targets"].as<JsonArray>();
    if (arr.isNull()) {
        Serial.println("dxmon_fetch_targets: 'targets' field missing or not an array");
        return false;
    }

    // PSRAM-backed, not `static` in internal DRAM -- see do_full_refresh()'s matching
    // comment in main.cpp for the full real bug this fixes (a link-time DRAM overflow
    // once combined with the LVGL pool increase in lv_conf.h). Allocated once, lazily.
    static TargetsData *temp = nullptr;
    if (!temp) {
        temp = (TargetsData *)heap_caps_malloc(sizeof(TargetsData), MALLOC_CAP_SPIRAM);
        if (!temp) {
            Serial.println("dxmon_fetch_targets: PSRAM allocation for temp failed");
            return false;
        }
    }
    temp->count = 0;
    copy_field(temp->updated, sizeof(temp->updated), doc["updated"]);

    for (JsonVariant item : arr) {
        if (temp->count >= MAX_TARGET_ENTRIES) {
            Serial.println("dxmon_fetch_targets: MAX_TARGET_ENTRIES exceeded, truncating");
            break;
        }
        TargetEntry &t = temp->entries[temp->count];

        copy_field(t.type, sizeof(t.type), item["type"]);
        copy_field(t.entity, sizeof(t.entity), item["entity"]);
        copy_field(t.prefix, sizeof(t.prefix), item["prefix"]);
        copy_field(t.band, sizeof(t.band), item["band"]);
        copy_field(t.mode, sizeof(t.mode), item["mode"]);

        JsonVariant adxo = item["adxo"];
        if (adxo.isNull()) {
            t.has_adxo = false;
            t.adxo_active = false;
            t.adxo_begin[0] = '\0';
            t.adxo_end[0] = '\0';
        } else {
            t.has_adxo = true;
            t.adxo_active = adxo["active"] | false;
            copy_field(t.adxo_begin, sizeof(t.adxo_begin), adxo["begin"]);
            copy_field(t.adxo_end, sizeof(t.adxo_end), adxo["end"]);
        }

        parse_spot_info(t.last_spot, item["last_spot"]);
        parse_spot_info(t.last_seen, item["last_seen"]);

        temp->count++;
    }

    out = *temp;
    return true;
}

bool dxmon_fetch_watched(WatchedData &out)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("dxmon_fetch_watched: Wi-Fi not connected");
        return false;
    }

    HTTPClient http;
    String url = String("http://") + DXMON_SERVER_HOST + ":" + DXMON_SERVER_PORT + DXMON_WATCHED_PATH;
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("dxmon_fetch_watched: HTTP GET failed, code %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // ArduinoJson v7 -- a single JsonDocument, no fixed-size template/constructor
    // argument needed (a real API change from v6, confirmed via direct research
    // rather than assumed from possibly-stale training knowledge).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("dxmon_fetch_watched: JSON parse failed: %s\n", err.c_str());
        return false;
    }

    JsonArray watched = doc["watched"].as<JsonArray>();
    if (watched.isNull()) {
        Serial.println("dxmon_fetch_watched: no 'watched' array in response");
        return false;
    }

    static WatchedData temp;
    temp.count = 0;
    copy_field(temp.updated, sizeof(temp.updated), doc["updated"]);

    for (JsonObject entry : watched) {
        if (temp.count >= MAX_WATCHED_ENTRIES) break;
        WatchedEntry &we = temp.entries[temp.count];

        copy_field(we.callsign, sizeof(we.callsign), entry["callsign"]);
        copy_field(we.dxcc, sizeof(we.dxcc), entry["dxcc"]);

        JsonVariant adxo = entry["adxo"];
        we.adxo_active = !adxo.isNull() && (adxo["active"] | false);
        copy_field(we.adxo_end, sizeof(we.adxo_end), adxo["end"]);

        JsonVariant last_spot = entry["last_spot"];
        if (last_spot.isNull()) {
            we.has_last_spot = false;
            we.band[0] = '\0';
            we.mode[0] = '\0';
            we.frequency[0] = '\0';
            we.received_at[0] = '\0';
            we.comment[0] = '\0';
        } else {
            we.has_last_spot = true;
            copy_field(we.band, sizeof(we.band), last_spot["band"]);
            copy_field(we.mode, sizeof(we.mode), last_spot["mode"]);
            copy_field(we.frequency, sizeof(we.frequency), last_spot["frequency"]);
            copy_field(we.received_at, sizeof(we.received_at), last_spot["received_at"]);
            copy_field(we.comment, sizeof(we.comment), last_spot["comment"]);
        }

        temp.count++;
    }

    out = temp;
    return true;
}
