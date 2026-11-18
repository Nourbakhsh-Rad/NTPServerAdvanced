//-----------------------------------------------------------------------------
// Example: Industrial NTP Server v4.1.0 with Response Quality Filter
// Platform: ESP32 (Arduino Framework)
// Features: GPS Time Source, Response Quality Monitoring, Auth, Rate Limiting
//-----------------------------------------------------------------------------

#ifndef ESP32
#error "This example targets ESP32. On ESP8266 use ESP8266WiFi.h, and replace gettimeofday()/mbedtls with your own time source and MD5."
#endif

#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include <math.h>

#include "NTPServer_Advanced.h"


// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
const char *WIFI_SSID = "YOUR_SSID";
const char *WIFI_PASSWORD = "YOUR_PASSWORD";

// NTP Server instance
AdvancedNTPServer ntp;

// ---------------------------------------------------------------------------
// Time Source (GPS or RTC) — Replace with your actual time source
// ---------------------------------------------------------------------------
// For demonstration, we use system time set via NTP client at boot.
// In production, replace with GPS PPS, DS3231, or other high-accuracy source.

uint32_t getUTC() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

uint32_t getMicroseconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_usec; // 0 - 999999
}

// ---------------------------------------------------------------------------
// Optional: Temperature Sensor for RTC Compensation
// ---------------------------------------------------------------------------
float readTemperature() {
    // Replace with actual sensor (e.g., DS18B20, BME280, internal ESP32 temp)
    // return temperatureRead();
    return 25.0f; // Placeholder
}

// ---------------------------------------------------------------------------
// Optional: Authentication Handler (Symmetric Key MD5)
// ---------------------------------------------------------------------------
// NOTE: plain MD5(secret || data) is what RFC 5905's legacy symmetric-key
// scheme specifies, and is kept here for compatibility with that scheme.
// It is NOT a modern MAC — if you don't need interop with legacy NTP
// clients, prefer HMAC-SHA256 (mbedtls_md_hmac) in your own callback
// instead. The digest length must match NTP_AUTH_DIGEST_LEN.
#include <mbedtls/md5.h>

bool authHandler(uint32_t keyId, const uint8_t *data, size_t len, uint8_t *digest) {
    if (keyId != 1)
        return false;

    const char *secret = "MySecretKey123";
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const uint8_t *)secret, strlen(secret));
    mbedtls_md5_update(&ctx, data, len);
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Industrial NTP Server v4.1.0 Example ===\n");

    // Connect to WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // -----------------------------------------------------------------------
    // 1. Initialize NTP Server
    // -----------------------------------------------------------------------
    bool ok = ntp.begin(123, getUTC, getMicroseconds, nullptr, readTemperature);
    if (!ok) {
        Serial.println("[FATAL] Failed to start NTP server!");
        while (1)
            delay(1000);
    }

    // -----------------------------------------------------------------------
    // 2. Set Operating Mode
    // -----------------------------------------------------------------------
    ntp.setMode(NTPMode_t::SYNC);

    // -----------------------------------------------------------------------
    // 3. Response Quality Filter (renamed from "Clock Filter" in v3.5.0 —
    //    see README §6 for what this actually measures)
    // -----------------------------------------------------------------------
    ntp.setClockFilterEnabled(true);

    // -----------------------------------------------------------------------
    // 4. Security: Rate Limiting
    // -----------------------------------------------------------------------
    ntp.setRateLimitEnabled(true);
    ntp.setRateLimitParams(2, 5); // 2/sec rate, burst of 5

    // -----------------------------------------------------------------------
    // 5. Security: Authentication (Optional)
    // -----------------------------------------------------------------------
    // ntp.setAuthEnabled(true);
    // ntp.setAuthCallback(authHandler);

    // -----------------------------------------------------------------------
    // 6. Multicast / Broadcast (Optional)
    // -----------------------------------------------------------------------
    // ntp.setMulticastEnabled(true, 64);

    // -----------------------------------------------------------------------
    // 7. Temperature Compensation for RTC (Optional)
    // -----------------------------------------------------------------------
    // ntp.setTemperatureCompensation(-0.036f, 25.0f);

    // -----------------------------------------------------------------------
    // 8. Leap Second Event (Optional)
    // -----------------------------------------------------------------------
    // ntp.addLeapEvent(1767225599, +1);

    // -----------------------------------------------------------------------
    // 9. Server-side poll interval bounds (New in v3.5.0)
    // -----------------------------------------------------------------------
    ntp.setPollLimits(NTP_POLL_MIN, NTP_POLL_MAX); // 16s .. 1024s

    // -----------------------------------------------------------------------
    // 10. Measure System Precision
    // -----------------------------------------------------------------------
    int8_t prec = ntp.measurePrecision();
    Serial.printf("System precision: %d (%.3f us)\n", prec, pow(2.0, prec) * 1e6);

    // -----------------------------------------------------------------------
    // 11. Export Config for Persistence (Optional)
    // -----------------------------------------------------------------------
    ntp_config_t cfg;
    ntp.exportConfig(cfg);
    Serial.printf("Config: stratum=%d rootDelay=%.3f rootDisp=%.3f RQF=%d\n", cfg.stratum, cfg.rootDelaySec,
                  cfg.rootDispersionSec, cfg.clockFilterEnabled);

    Serial.println("\n=== NTP Server Ready ===\n");
}

// ---------------------------------------------------------------------------
// Main Loop — Monitoring & Diagnostics
// ---------------------------------------------------------------------------
void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastAssocPrint = 0;
    static uint32_t lastSelfTest = 0;

    uint32_t now = millis();

    // Lightweight health check every 60 seconds (new in v4.0.0). Cheap
    // enough to run from a watchdog task in production — this just
    // demonstrates reading the result.
    if (now - lastSelfTest >= 60000) {
        lastSelfTest = now;
        ntp_selftest_result_t st = ntp.selfTest();
        if (!st.ok) {
            Serial.printf("[selfTest] %s\n", st.message);
        }
    }

    // Print global statistics every 10 seconds
    if (now - lastPrint >= 10000) {
        lastPrint = now;

        ntp_statistics_t stats = ntp.getStatistics();
        Serial.println("\n--- Global Statistics ---");
        Serial.printf("Uptime: %lu sec\n", (unsigned long)stats.uptimeSeconds);
        Serial.printf("Packets RX: %u\n", stats.packetsReceived);
        Serial.printf("Packets TX: %u\n", stats.packetsSent);
        Serial.printf("Dropped: %u (rate limit)\n", stats.packetsDropped);
        Serial.printf("Invalid: %u\n", stats.packetsInvalid);
        Serial.printf("Auth Fail: %u\n", stats.packetsAuthFail);
        Serial.printf("KoD Sent: %u\n", stats.kissOfDeathSent);
        Serial.printf("Broadcast: %u\n", stats.broadcastSent);
        Serial.printf("Drift: %.3f ppm\n", stats.estimatedDriftPPM);
        Serial.printf("Clock Quality: %d\n", (int)stats.clockQuality);
    }

    // Print Response Quality Filter state per client every 30 seconds
    if (now - lastAssocPrint >= 30000) {
        lastAssocPrint = now;

        ntp_association_t assocList[8];
        size_t count = 0;
        ntp.getAssociations(assocList, 8, count);

        if (count > 0) {
            Serial.println("\n--- Client Associations (with Response Quality Filter) ---");
            for (size_t i = 0; i < count; i++) {
                ntp_association_t &a = assocList[i];
                IPAddress ip(a.ip);

                Serial.printf("\nClient #%zu: %s\n", i + 1, ip.toString().c_str());
                Serial.printf(" Packets: RX=%u TX=%u\n", a.packetsReceived, a.packetsSent);
                Serial.printf(" Raw one-way estimate (server-observed, NOT true client offset): %ld us\n",
                              (long)a.lastOffsetUs);
                Serial.printf(" Raw delay estimate: %ld us\n", (long)a.lastDelayUs);
                Serial.printf(" Version: %u Poll: %d\n", a.version, a.lastPoll);

#if NTP_ENABLE_CLOCK_FILTER
                Serial.printf(" --- Response Quality Filter ---\n");
                Serial.printf(" Filtered Offset: %.6f sec (%.1f us)\n", a.filteredOffsetSec, a.filteredOffsetSec * 1e6);
                Serial.printf(" Filtered Delay: %.6f sec (%.1f us)\n", a.filteredDelaySec, a.filteredDelaySec * 1e6);
                Serial.printf(" Dispersion: %.6f sec\n", a.filteredDispersion);
                Serial.printf(" Jitter (RMS): %.6f sec (%.1f us)\n", a.filteredJitter, a.filteredJitter * 1e6);
                Serial.printf(" Valid Stages: %d/8\n", a.filterValid);

                ntp_clock_filter_t cf;
                if (ntp.getClockFilterState(i, cf)) {
                    Serial.printf(" Root Distance: %.6f sec\n", (cf.peerDelay / 2.0) + cf.peerDispersion);
                }
#endif
            }
        } else {
            Serial.println("\n--- No active clients ---");
        }
    }

    delay(100);
}
