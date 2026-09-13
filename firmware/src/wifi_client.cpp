#include "wifi_client.h"
#include <WiFi.h>
#include <Preferences.h>
#include "wifi_credentials.h"
#include "config.h"

// NVS namespace/keys for stored Wi-Fi credentials -- matches the series' own
// established pattern (PropMon 2026-07-16, ported unchanged to APRSMon
// 2026-08-02).
static const char *NVS_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";

static bool nvs_has_credentials(String &ssid_out, String &pass_out)
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true); // read-only
    ssid_out = prefs.getString(NVS_KEY_SSID, "");
    pass_out = prefs.getString(NVS_KEY_PASS, "");
    prefs.end();
    return ssid_out.length() > 0;
}

void wifi_client_save_credentials(const char *ssid, const char *password)
{
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false); // read-write
    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, password);
    prefs.end();
    Serial.println("Wi-Fi credentials saved to NVS");
}

static bool connect_and_wait(const char *ssid, const char *password, uint32_t timeout_ms)
{
    WiFi.begin(ssid, password);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    return WiFi.status() == WL_CONNECTED;
}

bool wifi_client_connect(uint32_t timeout_ms)
{
    WiFi.mode(WIFI_STA);

    String stored_ssid, stored_pass;
    if (nvs_has_credentials(stored_ssid, stored_pass)) {
        Serial.println("Connecting using stored NVS credentials");
        if (connect_and_wait(stored_ssid.c_str(), stored_pass.c_str(), timeout_ms)) {
            Serial.print("Wi-Fi connected, IP: ");
            Serial.println(WiFi.localIP());
            return true;
        }
        // Deliberately does NOT fall back to the hardcoded credentials here --
        // if stored real credentials fail (wrong password changed on the
        // router, network gone), silently reverting to old hardcoded ones
        // would be confusing, not helpful. Failure here is the real signal
        // that Wi-Fi Setup needs to be run again.
        Serial.println("Stored NVS credentials failed to connect");
        return false;
    }

    // One-time migration: no NVS credentials yet, fall back to the hardcoded
    // wifi_credentials.h values and seed NVS with them on success -- matches
    // PropMon's own proven pattern exactly, so Dan's existing hardcoded
    // credentials get transparently migrated on first boot running this
    // code, not lost or requiring a manual step.
    Serial.println("No stored credentials -- falling back to wifi_credentials.h");
    if (connect_and_wait(WIFI_SSID, WIFI_PASSWORD, timeout_ms)) {
        Serial.print("Wi-Fi connected, IP: ");
        Serial.println(WiFi.localIP());
        wifi_client_save_credentials(WIFI_SSID, WIFI_PASSWORD);
        return true;
    }

    Serial.println("Wi-Fi connect failed or timed out");
    return false;
}

bool wifi_client_try_credentials(const char *ssid, const char *password)
{
    // Real, confirmed bug this guards against (PropMon, 2026-07-17): without
    // an explicit disconnect first, the connect call can short-circuit to
    // "success" if the device is already connected to ANY network -- which
    // it normally is here, since the portal only ever runs after a normal
    // boot already connected using stored credentials. Without this,
    // validation would never actually try the newly submitted credentials
    // at all, and a wrong password would be silently accepted (and then
    // saved over the real one).
    WiFi.disconnect();
    delay(100);

    Serial.printf("Trying credentials for \"%s\"\n", ssid);
    bool ok = connect_and_wait(ssid, password, WIFI_CONNECT_TIMEOUT_MS);

    if (!ok) {
        // Second real, confirmed bug this guards against (PropMon,
        // 2026-07-17): a failed attempt left the Wi-Fi driver in a confused
        // state that then broke a later, unrelated reconnect attempt too,
        // unless the driver is explicitly told to give up first.
        WiFi.disconnect();
    }

    return ok;
}
