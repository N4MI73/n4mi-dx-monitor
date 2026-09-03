#include "dxmon_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

/**
 * Real bug found and fixed 2026-09-03: JsonDocument's DEFAULT allocator uses plain
 * malloc(), which draws from the ESP32-S3's limited internal heap -- NOT the board's
 * 8MB of PSRAM, contrary to an earlier (wrong) assumption in this file. This was
 * originally found parsing the old /api/dxmon/targets response (66+ entries); kept
 * here as sound general practice for /api/dxmon/needed too even though the unified,
 * curated list (2026-09-04) is expected to run much smaller -- a real allocator
 * mistake shouldn't have to be rediscovered if this list ever grows. Watched's own
 * small response (max 10 entries) doesn't need this and keeps using the default
 * allocator. Exact pattern from ArduinoJson's own v7 documentation
 * (arduinojson.org/v7/how-to/use-external-ram-on-esp32/).
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

bool dxmon_fetch_needed(NeededData &out)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("dxmon_fetch_needed: Wi-Fi not connected");
        return false;
    }

    HTTPClient http;
    String url = String("http://") + DXMON_SERVER_HOST + ":" + DXMON_SERVER_PORT + DXMON_NEEDED_PATH;
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("dxmon_fetch_needed: HTTP GET failed, code %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Uses PSRAM explicitly (see SpiRamAllocator above) -- kept as sound general
    // practice even though this response is now much smaller than the old Targets
    // feed, see the comment above SpiRamAllocator. Constructed here, locally, not
    // at file scope -- PSRAM isn't ready to use before setup() runs, so a global
    // allocator/document would crash at static-init time.
    SpiRamAllocator allocator;
    JsonDocument doc(&allocator);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("dxmon_fetch_needed: JSON parse failed: %s\n", err.c_str());
        return false;
    }

    JsonArray arr = doc["needed"].as<JsonArray>();
    if (arr.isNull()) {
        Serial.println("dxmon_fetch_needed: 'needed' field missing or not an array");
        return false;
    }

    // PSRAM-backed, not `static` in internal DRAM -- kept from the old Targets
    // pattern (see do_full_refresh()'s matching comment in main.cpp) even though
    // this struct is now much smaller at MAX_NEEDED_ENTRIES=30 -- no longer close
    // to the internal-SRAM budget the way the old 80-entry version was, but no
    // reason to move it back to `static` internal RAM either. Allocated once, lazily.
    static NeededData *temp = nullptr;
    if (!temp) {
        temp = (NeededData *)heap_caps_malloc(sizeof(NeededData), MALLOC_CAP_SPIRAM);
        if (!temp) {
            Serial.println("dxmon_fetch_needed: PSRAM allocation for temp failed");
            return false;
        }
    }
    temp->count = 0;
    copy_field(temp->updated, sizeof(temp->updated), doc["updated"]);

    for (JsonVariant item : arr) {
        if (temp->count >= MAX_NEEDED_ENTRIES) {
            Serial.println("dxmon_fetch_needed: MAX_NEEDED_ENTRIES exceeded, truncating");
            break;
        }
        NeededEntry &t = temp->entries[temp->count];

        copy_field(t.id, sizeof(t.id), item["id"]);
        copy_field(t.kind, sizeof(t.kind), item["kind"]);
        copy_field(t.entity, sizeof(t.entity), item["entity"]);
        copy_field(t.band, sizeof(t.band), item["band"]);
        copy_field(t.mode, sizeof(t.mode), item["mode"]);

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
