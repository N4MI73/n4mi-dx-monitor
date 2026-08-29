#include "wifi_client.h"
#include <WiFi.h>
#include "wifi_credentials.h"

bool wifi_connect(uint32_t timeout_ms)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Wi-Fi connected, IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }

    Serial.println("Wi-Fi connect failed or timed out");
    return false;
}
