#include "dxmon_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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

    WatchedData temp;
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
