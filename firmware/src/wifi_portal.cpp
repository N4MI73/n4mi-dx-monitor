// N4MI Desktop Instrument Series - DX Monitor
// wifi_portal.cpp
//
// Ported from PropMon's proven, hardware-tested version (2026-07-17),
// carrying forward both real bugs already found and fixed there rather than
// risking rediscovering them -- same successful approach APRSMon's own port
// (2026-08-02) already used, with zero new bugs found that time.
//
// Bug #1 (PropMon, 2026-07-17): scanning while the AP is already running
// fails outright and repeatedly -- a known ESP32 limitation (the radio
// can't reliably hop channels for a scan while also holding a fixed channel
// for the AP's beacon). The original retry loop made this much worse: it
// retried on every loop() pass with no backoff, hammering the radio
// hundreds of times per second, which left the Wi-Fi driver in a confused
// state that then broke an unrelated later reconnect attempt too.
// Structured so the scan runs FIRST, in plain STA mode, before the AP is
// ever started -- same pattern WiFiManager and similar libraries use, for
// the same reason. Trade-off: the AP doesn't exist yet during the scan, so
// a phone won't see it in its Wi-Fi list for a few seconds after Setup
// opens -- the device's own touchscreen stays responsive throughout
// regardless, since this is still an async, polled scan.
//
// Bug #2 (PropMon, 2026-07-17): a device already connected to any network
// could silently report validation "success" without ever trying the
// submitted credentials -- see wifi_client.h's own comment on
// wifi_client_try_credentials() for the fix (an explicit disconnect first).

#include "wifi_portal.h"
#include "wifi_client.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

// See main.cpp for the full explanation -- set to 1 to compile in
// diagnostic Serial output, 0 (default) to compile it out entirely.
#define DEBUG_VERBOSE 0

static const byte DNS_PORT = 53;
static DNSServer dnsServer;
static WebServer server(80);

static bool portal_active = false;
static bool portal_complete = false;
static bool ap_started = false; // true once the AP/DNS/web server are actually up (after the scan)
static WifiPortalStatus status = WifiPortalStatus::SCANNING;

// Scan retry backoff -- prevents hammering WiFi.scanNetworks() on every
// loop() pass if a scan fails for a genuinely transient reason. Gives up
// gracefully after SCAN_MAX_RETRIES rather than looping forever -- the AP
// still comes up with an empty network list, which at least lets the setup
// abandon-timeout eventually clean things up instead of leaving the device
// stuck.
#define SCAN_RETRY_BACKOFF_MS 500
#define SCAN_MAX_RETRIES      6
static uint8_t scan_retry_count = 0;
static uint32_t last_scan_attempt_ms = 0;

// Deduplicated network list, rebuilt once per completed scan. Capped at a
// reasonable size -- more than enough for any real household.
#define MAX_UNIQUE_NETWORKS 24
static String unique_ssids[MAX_UNIQUE_NETWORKS];
static bool unique_locked[MAX_UNIQUE_NETWORKS];
static uint8_t unique_count = 0;
static bool scan_results_ready = false;

static void rebuild_unique_network_list() {
    int n = WiFi.scanComplete();
    unique_count = 0;
    if (n <= 0) return;

    // Networks are deduplicated by SSID, keeping the first (strongest,
    // since scan results come back sorted by RSSI) occurrence -- this
    // matters concretely for any mesh Wi-Fi system (multiple access points
    // broadcasting the same network name), not just as a theoretical edge
    // case.
    for (int i = 0; i < n && unique_count < MAX_UNIQUE_NETWORKS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue; // hidden network, skip

        bool dup = false;
        for (uint8_t j = 0; j < unique_count; j++) {
            if (unique_ssids[j] == ssid) { dup = true; break; }
        }
        if (dup) continue;

        unique_ssids[unique_count] = ssid;
        unique_locked[unique_count] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        unique_count++;
    }

#if DEBUG_VERBOSE
    if (Serial) Serial.printf("[portal] scan complete: %d unique networks\n", unique_count);
#endif
}

// Colors match DXMon's own established device identity (main.cpp's
// COLOR_STATUS_GREEN/COLOR_STATUS_RED/COLOR_BG/COLOR_TEXT_PRIMARY) rather
// than PropMon's teal, so the phone-side setup page feels like part of the
// same product as the device itself.
static void send_setup_page(const char *error_msg) {
    String html;
    html.reserve(2400);
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>DXMon Setup</title><style>"
            "body{font-family:sans-serif;background:#0a0e14;color:#e8ecf1;padding:20px;max-width:420px;margin:0 auto;}"
            "h1{color:#3ddc97;font-size:20px;}"
            "p.reminder{font-size:14px;color:#ffb84a;background:#1a2233;padding:10px;border-radius:6px;}"
            "label{display:block;margin-top:14px;font-size:13px;color:#999;}"
            "select,input[type=password]{width:100%;padding:10px;margin-top:4px;font-size:16px;"
            "background:#1d2536;color:#e8ecf1;border:1px solid #444;border-radius:4px;box-sizing:border-box;}"
            "button{width:100%;padding:12px;font-size:16px;background:#3ddc97;color:#05121f;"
            "border:none;border-radius:4px;font-weight:bold;margin-top:18px;}"
            ".err{color:#e05c5c;margin:10px 0;padding:10px;background:#2a1414;border-radius:4px;}"
            "</style></head><body>";
    html += "<h1>DXMon Wi-Fi Setup</h1>";

    // Dan's explicit requirement, 2026-09-13: this is the one place someone
    // is actually looking at the moment they pick a network, so it matters
    // more here than the device's own on-screen copy.
    html += "<p class='reminder'>Connect DXMon to the same Wi-Fi network your "
            "DXMon server (NAS) is running on -- otherwise it won't be able "
            "to reach it.</p>";

    if (error_msg) {
        html += "<div class='err'>" + String(error_msg) + "</div>";
    }

    if (unique_count == 0) {
        html += "<p>No networks found. Move the device closer to your router, "
                "then reconnect to this network and reload this page.</p>";
    } else {
        html += "<form method='POST' action='/connect'>";
        html += "<label>Network</label><select name='ssid'>";
        for (uint8_t i = 0; i < unique_count; i++) {
            html += "<option value='" + unique_ssids[i] + "'>" + unique_ssids[i];
            if (unique_locked[i]) html += " &#128274;";
            html += "</option>";
        }
        html += "</select>";
        html += "<label>Password</label>"
                "<input type='password' name='pass' autocomplete='off'>";
        html += "<button type='submit'>Connect</button>";
        html += "</form>";
    }
    html += "</body></html>";

    server.send(200, "text/html", html);
}

static void handle_root() {
    send_setup_page(nullptr);
}

static void handle_connect() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    if (ssid.length() == 0) {
        send_setup_page("Please choose a network.");
        return;
    }

    status = WifiPortalStatus::CONNECTING;
#if DEBUG_VERBOSE
    if (Serial) Serial.printf("[portal] validating credentials for \"%s\"\n", ssid.c_str());
#endif

    // Deliberately blocking, up to WIFI_CONNECT_TIMEOUT_MS -- see
    // wifi_portal.h's header comment. wifi_client_try_credentials() does
    // NOT touch WiFi.mode(), so the AP interface (and this phone's
    // connection to it) stays alive throughout the attempt.
    bool ok = wifi_client_try_credentials(ssid.c_str(), pass.c_str());

    if (ok) {
        wifi_client_save_credentials(ssid.c_str(), pass.c_str());
        status = WifiPortalStatus::CONNECTED;
        portal_complete = true;
#if DEBUG_VERBOSE
        if (Serial) Serial.println("[portal] connected, credentials saved");
#endif
        server.send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
            "<body style='font-family:sans-serif;background:#0a0e14;color:#e8ecf1;padding:20px;'>"
            "<h1 style='color:#3ddc97;'>Connected!</h1>"
            "<p>You can close this page. DXMon will start showing live data shortly.</p>"
            "</body></html>");
    } else {
        status = WifiPortalStatus::FAILED;
#if DEBUG_VERBOSE
        if (Serial) Serial.println("[portal] connection FAILED");
#endif
        send_setup_page("Could not connect -- check the password and try again.");
    }
}

static void start_ap_and_server() {
    if (ap_started) return;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SETUP_AP_NAME); // open network, no password

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", apIP);

    server.on("/", HTTP_GET, handle_root);
    server.on("/connect", HTTP_POST, handle_connect);
    server.onNotFound(handle_root); // catches every OS's captive-portal probe URL
    server.begin();

    ap_started = true;
    status = WifiPortalStatus::WAITING;

#if DEBUG_VERBOSE
    if (Serial) Serial.printf("[portal] AP started (post-scan), AP=\"%s\" IP=%s\n",
        WIFI_SETUP_AP_NAME, apIP.toString().c_str());
#endif
}

void wifi_portal_start() {
    if (portal_active) return;

    portal_active = true;
    portal_complete = false;
    ap_started = false;
    status = WifiPortalStatus::SCANNING;
    scan_results_ready = false;
    unique_count = 0;
    scan_retry_count = 0;

    // Scan BEFORE the AP exists -- see file header for why. Plain STA mode
    // here (not AP_STA yet) avoids the channel conflict entirely.
    WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(true);
    last_scan_attempt_ms = millis();

#if DEBUG_VERBOSE
    if (Serial) Serial.println("[portal] scanning (AP not started yet)");
#endif
}

void wifi_portal_stop() {
    if (!portal_active) return;

    if (ap_started) {
        server.stop();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
    }

    // Explicitly drop back to pure STA mode, whether or not the AP ever
    // actually started this time.
    WiFi.mode(WIFI_STA);

    portal_active = false;
    portal_complete = false;
    ap_started = false;
    status = WifiPortalStatus::SCANNING;

#if DEBUG_VERBOSE
    if (Serial) Serial.println("[portal] stopped");
#endif
}

void wifi_portal_process() {
    if (!portal_active) return;

    if (!scan_results_ready) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            rebuild_unique_network_list();
            scan_results_ready = true;
            start_ap_and_server();
        } else if (n == WIFI_SCAN_FAILED) {
            if (scan_retry_count >= SCAN_MAX_RETRIES) {
                // Give up gracefully rather than looping forever -- bring
                // the AP up anyway with an empty network list. The person
                // can still reload the page later (unlikely to help without
                // a real fix, but doesn't hang), and the setup
                // abandon-timeout eventually cleans up if nothing else does.
#if DEBUG_VERBOSE
                if (Serial) Serial.println("[portal] scan retries exhausted, starting AP anyway");
#endif
                scan_results_ready = true;
                unique_count = 0;
                start_ap_and_server();
            } else if ((millis() - last_scan_attempt_ms) >= SCAN_RETRY_BACKOFF_MS) {
                scan_retry_count++;
#if DEBUG_VERBOSE
                if (Serial) Serial.printf("[portal] scan failed, retry %d/%d\n",
                    scan_retry_count, SCAN_MAX_RETRIES);
#endif
                WiFi.scanNetworks(true);
                last_scan_attempt_ms = millis();
            }
            // else: still within backoff window, do nothing this pass
        }
        return; // AP/DNS/web server don't exist yet during the scan phase
    }

    dnsServer.processNextRequest();
    server.handleClient();
}

bool wifi_portal_is_active() {
    return portal_active;
}

bool wifi_portal_is_complete() {
    return portal_complete;
}

WifiPortalStatus wifi_portal_get_status() {
    return status;
}

uint8_t wifi_portal_network_count() {
    return unique_count;
}

uint8_t wifi_portal_client_count() {
    if (!ap_started) return 0;
    return WiFi.softAPgetStationNum();
}

IPAddress wifi_portal_get_ap_ip() {
    if (!ap_started) return IPAddress(0, 0, 0, 0);
    return WiFi.softAPIP();
}
