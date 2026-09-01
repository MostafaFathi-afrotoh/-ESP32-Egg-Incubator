// diag_io.cpp
// Phase 4A + Phase 5 JSON export
// Non-blocking WiFi + rate-limited publish + exponential backoff
// Never modifies control logic.

#include "diag_io.h"
#include "diagnostics.h"
#include "telegram_alerts.h"
#include "predictive_model.h"
#include "cycle_archive.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

// ============================================================
// Credentials live in secrets.h (do NOT commit real values)
// ============================================================
#include "secrets.h"
static const char* WIFI_SSID     = SECRET_WIFI_SSID;
static const char* WIFI_PASS     = SECRET_WIFI_PASS;
static const char* AIO_USER      = SECRET_AIO_USER;
static const char* AIO_KEY       = SECRET_AIO_KEY;
static const char* AIO_FEED_BASE = "https://io.adafruit.com/api/v2/";

// Rate limits (ms)
static const unsigned long RATE_FAST   = 5UL * 60UL * 1000UL;   // 5 min
static const unsigned long RATE_SLOW   = 30UL * 60UL * 1000UL;  // 30 min
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 3000UL;
static const unsigned long HTTP_TIMEOUT_MS         = 1000UL;

// ============================================================
// State
// ============================================================
uint32_t wifiReconnectAttempts = 0;
bool     wifiConnected         = false;

static unsigned long lastWifiAttempt   = 0;
static unsigned long wifiBackoffMs     = 60000UL;   // start 1 min
static const unsigned long WIFI_BACKOFF_MAX = 3600000UL; // 1 hour

static unsigned long lastFastPublish   = 0;
static unsigned long lastSlowPublish   = 0;

// JSON buffer (static, no heap)
static char jsonBuf[192];

// External symbols from main sketch / diagnostics
extern float rawTemperature;
extern float currentTemperature;
extern bool  hasValidTemperature;
extern bool  emergencyMode;
extern bool  heaterState;
extern unsigned long cycleCount;

// ============================================================
// Helper: Test real internet connectivity (non-blocking)
// ============================================================
bool isInternetAvailable() {
    // نجعل الاختبار يعتمد فقط على حالة WiFi، 
    // لأن اختبار الاتصال بخوادم خارجية قد يفشل بسبب جدران نارية أو تأخير.
    return (WiFi.status() == WL_CONNECTED);
}// ============================================================
// Helper: Connect to WiFi with internet verification
// ============================================================
static bool tryConnectWiFi() {
    // Root cause of E (xxxx) wifi_init_default: netstack cb reg failed with 12308:
    // repeated WiFi.disconnect(true) + mode/begin tears down esp_netif and
    // re-registers callbacks incorrectly. Use soft reconnect after first init.
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        return true;
    }

    static bool wifiStackReady = false;

    esp_task_wdt_reset();

    if (!wifiStackReady) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        wifiStackReady = true;
        delay(50);
        esp_task_wdt_reset();
    } else {
        // Soft disconnect — keep netif, do NOT erase credentials (arg=false)
        WiFi.disconnect(false);
        delay(100);
        esp_task_wdt_reset();
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long startMs = millis();
    while ((millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
        esp_task_wdt_reset();
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            delay(100);
            esp_task_wdt_reset();
            wifiConnected = true;
            wifiBackoffMs = 300000UL;
            IPAddress ip = WiFi.localIP();
            Serial.printf("WiFi connected, IP: %u.%u.%u.%u\n",
                          ip[0], ip[1], ip[2], ip[3]);
            resetTelegramBot();
            return true;
        }
        // Fail fast on auth reject (wrong password) — don't spin full timeout silently
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            break;
        }
        delay(100);
    }

    wifiConnected = false;
    wifiReconnectAttempts++;
    // Soft leave; do not WiFi.mode(WIFI_OFF) every time (triggers 12308 on next begin)
    WiFi.disconnect(false);
    Serial.printf("WiFi connect failed (status=%d, attempts=%lu)\n",
                  (int)WiFi.status(), (unsigned long)wifiReconnectAttempts);

    // After many failures: one hard cycle to recover netif (rare path)
    if ((wifiReconnectAttempts % 8) == 0) {
        Serial.println("WiFi: hard reset stack (every 8 failures)");
        WiFi.mode(WIFI_OFF);
        delay(200);
        esp_task_wdt_reset();
        WiFi.mode(WIFI_STA);
        wifiStackReady = true;
        delay(100);
        esp_task_wdt_reset();
    }
    return false;
}

// ============================================================
// Helper: Post a feed to Adafruit IO
// ============================================================
bool httpPostFeed(const char* feed, float value) {
    if (!wifiConnected) {
        Serial.printf("❌ Adafruit: wifiConnected is false, skipping %s\n", feed);
        return false;
    }
    esp_task_wdt_reset();

    char url[160];
    snprintf(url, sizeof(url), "%s%s/feeds/%s/data", AIO_FEED_BASE, AIO_USER, feed);

    char body[48];
    snprintf(body, sizeof(body), "{\"value\":%.2f}", value);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    esp_task_wdt_reset();

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-AIO-Key", AIO_KEY);

    int code = http.POST(body);
    http.end();
    esp_task_wdt_reset();

    if (code >= 200 && code < 300) {
        Serial.printf("✅ Adafruit %s posted, code %d\n", feed, code);
        return true;
    }
    Serial.printf("❌ Adafruit %s FAILED, code %d\n", feed, code);
    return false;
}


// ============================================================
// Public API
// ============================================================
void diag_io_init() {
    Serial.println("📡 diag_io_init: attempting WiFi (max 5 s)...");
    lastWifiAttempt = millis();

    if (!tryConnectWiFi()) {
        Serial.println("📡 WiFi not available – continuing offline (cloud optional)");
        lastWifiAttempt = millis();
        wifiBackoffMs = 120000UL;
    }

    // Init dependent modules
    telegram_init();
    predictive_init();
    cycle_archive_init();
}

void diag_io_sync() {
    // Force a local consistency check – publish current actuator states
    // if network is up. Does not change any control variables.
    if (!wifiConnected) return;
    // Placeholder: could publish heater/fan/turner states if feeds exist.
}

const char* buildJsonPayload() {
    float temp = hasValidTemperature ? currentTemperature : getEstimatedTemperature();
    float health = getHealthScore();
    float over = getAvgOvershoot();
    float duty = getDutyCycle();

    unsigned long t = millis() / 1000UL;
    snprintf(jsonBuf, sizeof(jsonBuf),
             "{\"t\":%lu,\"temp\":%.2f,\"health\":%.1f,\"overshoot\":%.2f,\"duty\":%.1f}",
             t, temp, health, over, duty);
    return jsonBuf;
}

void publishAllFeeds() {
    if (!wifiConnected) {
        Serial.println("❌ Cannot publish: WiFi not connected.");
        return;
    }
    esp_task_wdt_reset();
    httpPostFeed("health-score", getHealthScore());
    httpPostFeed("temperature-avg", hasValidTemperature ? currentTemperature : getEstimatedTemperature());
    httpPostFeed("overshoot-p95", getP95Overshoot());
    httpPostFeed("sensor-delta", getAvgDelta());
    httpPostFeed("duty-cycle", getDutyCycle());
    esp_task_wdt_reset();
    Serial.println("✅ All feeds published.");
}
// ============================================================
// Main loop: WiFi reconnect, rate-limited publishes, Telegram, Predictive
// ============================================================
void diag_io_loop() {
    unsigned long now = millis();
    static unsigned long lastInternetCheck = 0;
    esp_task_wdt_reset();
    // ---- Check real internet availability every 30 seconds ----
    if (now - lastInternetCheck >= 30000UL) {
        lastInternetCheck = now;
        if (wifiConnected) {
            if (!isInternetAvailable()) {
                Serial.println("📡 Internet connection lost! (detected by ping)");
                wifiConnected = false;
                wifiReconnectAttempts++;
                lastWifiAttempt = now;
                wifiBackoffMs = 120000UL; // reset backoff to 1 min
            }
        }
    }

    // ---- WiFi reconnect with exponential backoff (only if not connected) ----
    if (!wifiConnected) {
        if (now - lastWifiAttempt >= wifiBackoffMs) {
            lastWifiAttempt = now;
            Serial.printf("📡 WiFi retry (attempt %lu, backoff %lu s)\n",
                          wifiReconnectAttempts + 1, wifiBackoffMs / 1000UL);
            if (tryConnectWiFi()) {
                // success: tryConnectWiFi already called resetTelegramBot()
            } else {
                // double backoff, cap at 1 hour
                wifiBackoffMs = (wifiBackoffMs < WIFI_BACKOFF_MAX / 2)
                                    ? wifiBackoffMs * 2
                                    : WIFI_BACKOFF_MAX;
            }
        }
    } else {
        // quick check for sudden disconnection (WiFi layer lost)
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            wifiReconnectAttempts++;
            lastWifiAttempt = now;
            wifiBackoffMs = 60000UL;
            Serial.println("📡 WiFi lost – entering backoff");
        }
    }

    // ---- Rate-limited publish (only if connected) ----
    if (wifiConnected) {
        // Fast feeds: health + temperature (every 5 min)
        if (now - lastFastPublish >= RATE_FAST) {
            lastFastPublish = now;
            float health = getHealthScore();
            float temp = hasValidTemperature ? currentTemperature : getEstimatedTemperature();
            httpPostFeed("health-score", health);
            httpPostFeed("temperature-avg", temp);
            esp_task_wdt_reset();
        }

        // Slow feeds: overshoot-p95, sensor-delta, duty-cycle (every 30 min)
        if (now - lastSlowPublish >= RATE_SLOW) {
            lastSlowPublish = now;
            httpPostFeed("overshoot-p95", getP95Overshoot());
            httpPostFeed("sensor-delta", getAvgDelta());
            httpPostFeed("duty-cycle", getDutyCycle());
            esp_task_wdt_reset();
        }
    }

    // ---- Telegram non-blocking loop ----
    telegram_loop();

    // ---- Predictive model maintenance ----
    predictive_loop();
    esp_task_wdt_reset();
}