# NTPServerAdvanced — RFC 5905 NTP Server for ESP32 / ESP8266

## Complete Technical Documentation

**Version:** 4.1.0  
**Standard:** RFC 5905 (NTPv4) — server-side subset; see §6 for the honest scope of the diagnostic filter  
**Platform:** ESP32 / ESP8266 (Arduino Framework)  
**Language:** `C++11/C++14`  
**Dependencies:** `AsyncUDP` / `ESPAsyncUDP`, `lwip`, `math.h`

> **v4.1.0 is a correctness release — please upgrade if you use authentication,
> Kiss-o'-Death, or leap seconds.** Earlier versions hashed the wrong bytes
> when verifying a MAC (no real client could authenticate), never signed
> replies, put `refId` on the wire byte-swapped (so `RATE`/`AUTH` KoD codes and
> `GPS` were unreadable), sent KoD packets clients discard as bogus, and
> never applied the leap callback/table to the LI field. All fixed and covered
> by end-to-end packet tests; see §13.
>
> Earlier breaking change (4.0.0): `setPeerTxTimestamp()` was removed —
> Interleaved Mode is automatic and per-client (§5.9).

---

## Table of Contents

1. [Introduction & Overview](#1-introduction--overview)
2. [Features & Capabilities](#2-features--capabilities)
3. [Architecture & Code Structure](#3-architecture--code-structure)
4. [Quick Start](#4-quick-start)
5. [API Reference](#5-api-reference)
6. [Response Quality Filter](#6-response-quality-filter-formerly-clock-filter)
7. [Advanced Configuration](#7-advanced-configuration)
8. [Security](#8-security)
9. [Usage Examples](#9-usage-examples)
10. [Limitations & Performance Considerations](#10-limitations--performance-considerations)
11. [Troubleshooting](#11-troubleshooting)
12. [RFC 5905 Compliance Matrix](#12-rfc-5905-compliance-matrix)
13. [Changelog](#13-changelog)
14. [Testing & CI](#14-testing--ci)

---

## 1. Introduction & Overview

This library is an **industrial-grade, advanced** NTP (Network Time Protocol) version 4 server implementation designed to comply with the **server-side portions of RFC 5905** and optimized for embedded systems based on ESP32 and ESP8266

Version 4.0.0 fixes the remaining architectural rough edges from 3.5.0: Interleaved Mode is now tracked per client instead of through one shared server-wide timestamp, the extension-field parser walks past unrecognized RFC 7822 TLV fields instead of assuming the legacy MAC is the only thing after the header, and temperature compensation actually corrects outgoing timestamps instead of only computing a discarded value. It also adds a `selfTest()` health check, additional standard Kiss-o'-Death codes, official PlatformIO/Arduino Library Manager metadata, and a native (host-buildable) unit test suite — see `tests/`

### Typical Use Cases

- Synchronizing IoT device clocks in industrial factories
- Local NTP server in air-gapped industrial networks
- NTP Secondary node in a synchronization hierarchy
- Distributing time from GPS/PPS to local clients
- Clock quality monitoring in SCADA systems

---

## 2. Features & Capabilities

### 2.1. Standards Compliance

| Feature                    | Status | Description                                     |
| -------------------------- | ------ | ----------------------------------------------- |
| NTPv4 Packet Format        | ✅     | 48-byte packed structure with `static_assert`   |
| NTP Short Format           | ✅     | rootDelay & rootDispersion in 16.16 fixed-point |
| Leap Indicator             | ✅     | Automatic leap second handling (LI=1/2)         |
| Stratum 0 (KoD)            | ✅     | Kiss-o'-Death with RATE/AUTH/CRYP/RSTR/DENY codes (expanded in 4.0.0) |
| Broadcast/Multicast        | ✅     | Periodic Mode=5 to 224.0.1.1                    |
| Control Messages (mode 6)  | ❌     | Counted and dropped; `ntpq` is not supported    |
| Interleaved Mode           | ✅     | Automatic per-client T3 tracking (fixed in 4.0.0 — previously one shared timestamp, which corrupted state with >1 concurrent interleaved client) |
| Extension Field Parsing    | ✅     | RFC 7822 TLV walker locates the legacy MAC even behind unrecognized extension fields (new in 4.0.0) |
| Server Poll-Interval Clamp | ✅     | Echoed poll clamped to `[NTP_POLL_MIN, NTP_POLL_MAX]` |
| Temperature Compensation   | ✅     | Linear PPM model, actually applied to outgoing timestamps (fixed in 4.0.0 — previously computed and discarded) |
| IPv6                       | ❌     | Not supported — see §10 |
| **Response Quality Filter**| **✅** | **8-stage shift register + spike detector — diagnostic tool, not RFC A.5.1.1 client discipline; see §6** |

### 2.2. Security

| Feature                        | Status                                    |
| ------------------------------ | ----------------------------------------- |
| **Token Bucket Rate Limiting** | ✅ Mitigates per-source-IP abuse (spoofed IPs can still exhaust tables — see §8) |
| **Symmetric Key Auth (MAC)**   | ✅ KeyID + digest verified over the wire bytes; replies are signed (fixed in 4.1.0) |
| **Kiss-o'-Death**              | ✅ RATE (rate limit), AUTH (bad MAC/key), CRYP (malformed extension field), RSTR (reserved for future policy use), DENY (fallback) |
| **Association Tracking**       | ✅ 32 clients with history + Response Quality Filter |
| **Thread-safe shared state**   | ✅ Critical section around associations/stats/buckets |
| **selfTest()**                 | ✅ Runtime consistency check for watchdog/health-check use (new in 4.0.0) |

### 2.3. Code Quality
| Aspect                | Status                                                        |
| ---------------------- | -------------------------------------------------------------- |
| **Native unit tests**   | ✅ `tests/test_ntp.cpp` — 99 assertions covering fixed-point math, token bucket, association table, extension-field walker, constant-time compare, microsecond folding, whole-packet behavior and era rollover (replies, KoD, authentication, leap indicator, drift). Runs on a desktop g++, no hardware needed |
| **CI**                  | ✅ `.github/workflows/tests.yml` — runs the test suite and sanity-compiles all four build configurations plus the example sketch on every push/PR |
| **Library packaging**   | ✅ `library.json` (PlatformIO) + `library.properties` (Arduino Library Manager) |
| **Versioning**          | ✅ `NTP_SERVER_VERSION_MAJOR/MINOR/PATCH/STRING` macros in the header |

### 2.4. Monitoring & Diagnostics

| Feature                      | Status                                             |
| ---------------------------- | -------------------------------------------------- |
| **Statistics**               | Counters for RX/TX/Drop/AuthFail/KoD/Uptime/Drift  |
| **Response Quality Filter Outputs** | Filtered Offset/Delay/Dispersion/Jitter per client (diagnostic — see §6) |
| **Drift Estimation**         | Automatic local clock drift PPM estimation         |
| **Temperature Compensation** | RTC drift compensation based on temperature        |
| **Syslog Forwarding**        | Event forwarding to Syslog server (compile-time)   |
| **Logging Levels**           | 5 levels: Off / Error / Warn / Info / Debug        |

### 2.5. Flexibility

| Feature                    | Status                                           |
| -------------------------- | ------------------------------------------------ |
| **Runtime Mode Switch**    | GPS / SYNC / VALID / ALARM                       |
| **Precision Auto-Measure** | Automatic precision measurement via `micros()`   |
| **Config Persistence**     | `ntp_config_t` structure for EEPROM/FS storage   |
| **Microsecond Callback**   | Microsecond precision support (0-999999)         |
| **Compile-time Features**  | Response Quality Filter, Syslog can be disabled to save RAM |

---

## 3. Architecture & Code Structure

### 3.1. Block Diagram

```
  ┌─────────────────────────────────────────┐
  │            AdvancedNTPServer            │
  ├─────────────────────────────────────────┤
  │ UDP Listener (Port 123)  [runs on the   │
  │  network stack's own task/context]      │
  │ ├── Packet Parser (48-byte validate)    │
  │ ├── Rate Limiter (Token Bucket)         │
  │ ├── Auth Verifier (constant-time MAC)   │
  │ ├── Timestamp Capture (T2)              │
  │ ├── Response Builder (T1/T2/T3/T4)      │
  │ ├── Poll-Interval Clamp                 │
  │ └── Multicast Sender (Async)            │
  ├─────────────────────────────────────────┤
  │ Response Quality Filter (diagnostic;    │
  │ math from RFC 5905 A.5.1.1 / A.5.2)     │
  │ ├── 8-Stage Shift Register per client   │
  │ ├── Sort by Delay (Wedge Scattergram)   │
  │ ├── Weighted Dispersion (epsilon)       │
  │ ├── RMS Jitter (psi)                    │
  │ └── Spike Detector (SGATE)              │
  ├─────────────────────────────────────────┤
  │ State Management  [critical-section     │
  │  guarded — see §3.4]                    │
  │ ├── Association Table (32 entries)      │
  │ ├── Token Bucket Table (32 entries)     │
  │ ├── Leap Event Table (8 entries)        │
  │ └── Drift Estimator (PPM)               │
  ├─────────────────────────────────────────┤
  │ Callback Interface  [called from the    │
  │  UDP task — keep them fast, no delay()] │
  │ ├── getUTC() → uint32_t (sec)           │
  │ ├── getSubSec() → uint32_t (μs)         │
  │ ├── getLeap() → uint8_t (LI)            │
  │ ├── getTemp() → float (°C)              │
  │ └── authHandler() → bool (MAC)          │
  └─────────────────────────────────────────┘
```

### 3.2. Files

| File                     | Description                                                         |
| ------------------------ | -------------------------------------------------------------------- |
| `src/NTPServer_Advanced.h` | Definitions, structures, enums, main class                          |
| `src/NTPServer_Advanced.cpp` | Packet processing, Response Quality Filter, rate limit, drift logic |
| `examples/NTPserver_RFC5905/` | Complete example with Response Quality Filter monitoring            |
| `library.json`           | PlatformIO Registry manifest                                        |
| `library.properties`     | Arduino Library Manager manifest                                    |
| `tests/test_ntp.cpp`     | Native (host-buildable) unit test suite — see §14                   |
| `tests/Makefile`         | `make run` builds and runs the tests                                |
| `tests/stubs/`           | Minimal Arduino/AsyncUDP headers so the tests build without an ESP toolchain |
| `.github/workflows/tests.yml` | CI: runs the tests and sanity-compiles every build configuration |

### 3.3. External Dependencies

```cpp
#include <Arduino.h>
#include <lwip/def.h>
#include <math.h>   // Required for Response Quality Filter (sqrt, log2, pow, fabs)

#ifdef ESP32
    #include <AsyncUDP.h>
#else
    #include <ESPAsyncUDP.h>
#endif
```

Note: For Authentication, you need an MD5/HMAC library (e.g., `mbedtls/md5.h` on ESP32) in your user callback

### 3.4. Concurrency Model

`AsyncUDP` (ESP32) / `ESPAsyncUDP` (ESP8266) invokes the packet callback
from the network stack's own task — not from the task that calls
`loop()`. In 3.1.1, the association table, statistics, and token-bucket
table were read and written from both contexts with no synchronization,
which is a data race (most visible as occasionally-corrupted stats or
association entries under load, worse on dual-core ESP32 where the two
tasks can genuinely run in parallel).

3.5.0 wraps every access to shared state in a short critical section
(`portENTER_CRITICAL`/`portEXIT_CRITICAL` on ESP32, `noInterrupts()`/
`interrupts()` on ESP8266). These sections are kept small — table lookups
and struct copies, not the packet I/O itself — so the impact on jitter is
minimal, but if you are extremely latency-sensitive, be aware every public
getter now briefly locks the same section the UDP callback uses.

---

## 4. Quick Start

### 4.0. Installation

**Arduino IDE:** download the repository as a ZIP and use *Sketch → Include Library → Add .ZIP Library*, or copy the folder to `Documents/Arduino/libraries/NTPServerAdvanced/`.
**PlatformIO:** add `lib_deps = https://github.com/Nourbakhsh-Rad/NTPServerAdvanced.git` to `platformio.ini`.

The library follows the standard layout (`src/`, `examples/`), so only `src/` is compiled into your project; `tests/` is never built by the Arduino or PlatformIO toolchains.

**ESP8266** additionally needs the [ESPAsyncUDP](https://github.com/me-no-dev/ESPAsyncUDP) library (on ESP32 `AsyncUDP` is part of the core). The bundled example targets ESP32.

### 4.1. Minimum Working Code
```cpp
#include <WiFi.h>
#include "NTPServer_Advanced.h"

AdvancedNTPServer ntp;

uint32_t getUTC() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

uint32_t getMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_usec;  // 0-999999
}

void setup() {
    Serial.begin(115200);
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    ntp.begin(123, getUTC, getMicros);
    ntp.setMode(NTPMode_t::VALID);

    // Response Quality Filter is enabled by default
    ntp.setClockFilterEnabled(true);
}

void loop() {
    delay(1000);
}
```

### 4.2. Expected Output
Clients on the local network should receive responses when running:
```bash
ntpdate -q 192.168.1.100
```

---

## 5. API Reference
### 5.1. Lifecycle Management
`AdvancedNTPServer()`  
Constructor. Initializes internal tables (associations, rate limits, leap events, response quality filters)  

`~AdvancedNTPServer()`  
Destructor. Automatically calls `end()`  

`bool begin(uint16_t port, ntp_utc_cb_t getUTC, ntp_subsec_cb_t getSubSec, ntp_leap_cb_t getLeap = nullptr, ntp_temp_cb_t getTemp = nullptr)`  

| Parameter   | Type                            | Description                                       |
| ----------- | ------------------------------- | ------------------------------------------------- |
| `port`      | `uint16_t`                      | UDP port (usually 123)                            |
| `getUTC`    | `std::function<uint32_t(void)>` | Seconds since 1970-01-01                          |
| `getSubSec` | `std::function<uint32_t(void)>` | Fractional seconds in microseconds (0 to 999999)  |
| `getLeap`   | Optional                        | Current LI (0-3). If null, leap table is used     |
| `getTemp`   | Optional                        | Ambient temperature in °C for compensation        |

Returns: `true` if UDP port binding succeeds  
Note: `getSubSec` must return μs, not ms. If your RTC only provides ms, multiply by 1000  

`void end()`  
Closes UDP socket and stops the service  

`bool isRunning() const`  
Service status

---

### 5.2. Server Control
`void suspend(bool suspend)`  
Temporarily suspends service. In suspend mode, packets are silently dropped  

`bool isSuspended() const`

---

### 5.3. Mode & Clock Quality Configuration
`void setMode(NTPMode_t mode)`  

| Value               | Stratum | RefID | Root Delay | Root Dispersion | LI |
| ------------------- | ------- | ----- | ---------- | --------------- | -- |
| `NTPMode_t::GPS`    | 1       | GPS   | 0.0        | 1ms             | 0  |
| `NTPMode_t::SYNC`   | 2       | LCLS  | 10ms       | 50ms            | 0  |
| `NTPMode_t::VALID`  | 5       | LCLV  | 100ms      | 500ms           | 0  |
| `NTPMode_t::ALARM`  | 0       | INIT  | 1.0s       | 16.0s           | 3  |

`void setClockQuality(ClockQuality_t q)`  

Records clock quality for internal monitoring:
- `UNKNOWN`, `UNSTABLE`, `WARMUP`, `GOOD`, `EXCELLENT`

`void setLeapIndicator(uint8_t li)`  
Value 0 to 3. It is recommended to use `addLeapEvent()` instead of manual setting  

`void setStratum(uint8_t stratum)`  
0 to 15. Values ≥16 are automatically converted to 0 in outgoing packets (RFC 5905)  

`void setRefID(const char* id)`  
4 ASCII characters. Example: `"GPS "`, `"LCLS"`  

`void setRootDelay(double seconds)`  
Round-trip delay to reference source. Example: `0.010` = 10 milliseconds  

`void setRootDispersion(double seconds)`  
Maximum error from reference source. Example: `0.050` = 50 milliseconds  

`void setPrecision(int8_t precision)`  
Value in log2(seconds). Example: `-18` ≈ 3.8 μs. If not set, `begin()` automatically calls `measurePrecision()`  

`int8_t measurePrecision()`  
Measures execution time of 2048 callback invocations (UTC + SubSec)  
Note: This function must be executed under zero network load

---

### 5.4. Authentication
`void setAuthEnabled(bool enabled)`  
Enables MAC verification. When active, incoming packets must contain at least 20 bytes of Extension (4-byte KeyID + Digest)

> **Fixed in 3.5.0:** the Key-ID used to be read from the *pointer address*
> of the extension field instead of its bytes, so verification never
> actually matched a real client MAC — `setAuthEnabled(true)` was
> effectively a no-op that rejected everyone (or, depending on your
> callback, could behave unpredictably). It now reads the 4 Key-ID
> bytes correctly and compares the digest in constant time. If you had
> authentication "working" against 3.1.1, re-test against 3.5.0+ with a
> real client — behavior has changed.
>
> **Fixed in 4.1.0:** the digest used to be computed over the packet *after*
> its fields had been converted to host byte order, so it never matched a
> real client's MAC on little-endian ESP chips. It is now computed over the
> received bytes. In addition, replies to authenticated requests now carry
> a MAC (KeyID + digest, 68 bytes total); clients configured with a key
> (ntpd/chrony `key N`) discard unsigned replies.
>
> **Improved in 4.0.0:** the code used to assume the legacy MAC was the
> only thing present after the 48-byte header. It now walks any RFC 7822
> TLV extension fields that precede the MAC (skipping ones it doesn't
> recognize) to find it — see §8.1 for what happens if that walk fails.

`void setAuthCallback(ntp_auth_cb_t cb)`  

| Callback Signature                                                                  | Description                                        |
| ----------------------------------------------------------------------------------- | -------------------------------------------------- |
| `bool handler(uint32_t keyId, const uint8_t* data, size_t len, uint8_t* outDigest)` | data = the packet bytes to authenticate **exactly as on the wire** (48-byte header, plus any extension fields preceding the MAC); outDigest = `NTP_AUTH_DIGEST_LEN` bytes (16 for MD5). The same callback is used to verify incoming MACs and to sign replies, so return `false` for unknown keys |

Implementation Example:

```cpp
#include <mbedtls/md5.h>

bool myAuthHandler(uint32_t keyId, const uint8_t* data, size_t len, uint8_t* digest) {
    if (keyId != 1) return false;
    const char* secret = "MySecretKey123";
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const uint8_t*)secret, strlen(secret));
    mbedtls_md5_update(&ctx, data, len);
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    return true;
}

// In setup:
ntp.setAuthCallback(myAuthHandler);
```

Warning: MD5 is not recommended for new cryptographic applications, but it is the traditional standard in NTPv4's symmetric-key scheme. If you don't need interop with legacy NTP clients, use HMAC-SHA256 in your own callback instead (set `NTP_AUTH_DIGEST_LEN` to 32). For NTS (TLS), a separate implementation is required — this library does not provide it

---

### 5.5. Rate Limiting (Token Bucket)
`void setRateLimitEnabled(bool enabled)`  
`void setRateLimitParams(uint32_t ratePerSec, uint32_t burst)`  

| Parameter    | Description                            |
| ------------ | -------------------------------------- |
| `ratePerSec` | Allowed requests per second (e.g., 2)  |
| `burst`      | Allowed burst size (e.g., 5)           |

Algorithm: Token Bucket. Each IP has a bucket that receives `rate` tokens per second (max `burst`). Each request consumes one token. If the bucket is empty, KoD RATE is sent

`void clearRateLimitTable()`  
Clears all token buckets

---

### 5.6. Multicast / Broadcast
`void setMulticastEnabled(bool enabled, uint32_t intervalSec = 64)`

Enables automatic broadcast/multicast packet transmission
 - Destination: `224.0.1.1` (NTP Multicast Group)
 - Mode: 5 (Broadcast)
 - Default interval: 64 seconds
 
`void sendBroadcastPacket()`  
Manually sends one broadcast packet

---

### 5.7. Leap Second
`void addLeapEvent(uint32_t unixTimestamp, int8_t type)`

| Parameter       | Description                                           |
| --------------- | ----------------------------------------------------- |
| `unixTimestamp` | Exact event time (Unix seconds)                       |
| `type`          | `+1` = add second (LI=1), `-1` = delete second (LI=2) |

Behavior: 24 hours before the event, the LI field of every reply (and broadcast) changes to 1 or 2. If you passed a `getLeap` callback to `begin()` it is authoritative instead and the table is ignored; a manual `setLeapIndicator()` warning or LI=3 (not in sync) always takes precedence. Applied as of 4.1.0 — earlier versions stored the events but never used them.
Example:
```cpp
// Illustrative only (no leap second is actually scheduled for this date):
// positive leap second at the end of Dec 31, 2025 UTC
ntp.addLeapEvent(1767225599, +1);
```
`void clearLeapEvents()`  
`uint8_t checkLeapSecond(uint32_t currentUnixTime)`

Manual check returning current LI

---

### 5.8. Temperature Compensation
`void setTemperatureCompensation(float ppmPerC, float refTempC)`

| Parameter  | Description                                                                 |
| ---------- | --------------------------------------------------------------------------- |
| `ppmPerC`  | RTC drift per degree Celsius (e.g., `-0.036` for typical 32.768kHz crystal) |
| `refTempC` | Calibration reference temperature (usually 25°C)                            |

`void setTempCallback(ntp_temp_cb_t cb)`  
Callback must return current temperature in °C

```
Note: Current implementation is simplified. For high accuracy, use a lookup table or 2nd-degree polynomial
```

---

### 5.9. Interleaved Mode (Symmetric Peers)
`void setInterleavedEnabled(bool enabled)`  
Enables Interleaved Mode. As of 4.0.0 this is the **only** call needed —
each client's previous transmit timestamp (T3) is tracked automatically
in that client's own association record
(`ntp_association_t::peerTxTimestamp`) and echoed back as `origTime` on
their *next* request, per RFC 5905's interleaved scheme. The first
response to any client always uses ordinary (non-interleaved) `origTime`,
since there is no prior T3 yet.

> **Breaking change from ≤3.5.0:** `setPeerTxTimestamp(const
> ntp_timestamp_t&)` has been removed. It stored a single value shared by
> every client, so two clients both using Interleaved Mode at the same
> time would corrupt each other's `origTime`. If you were calling it
> manually, just delete the call — the server now does this correctly on
> its own, per client.

### 5.10. Poll Interval Limits (new in 3.5.0)
`void setPollLimits(int8_t minPoll, int8_t maxPoll)`  
Clamps the poll interval this server echoes back to clients, instead of
blindly trusting whatever the client sent (RFC 5905 §7.2 leaves this up
to the server implementation). Defaults to `NTP_POLL_MIN` (4, 16s) and
`NTP_POLL_MAX` (10, 1024s). No-op if `minPoll > maxPoll`.

### 5.11. Self-Test (new in 4.0.0)
`ntp_selftest_result_t selfTest() const`  
A cheap, network-free consistency check: verifies the UTC/sub-second
callbacks are wired, precision is in a sane range, and the association
and token-bucket tables have no duplicate/invalid entries. Safe to call
periodically (e.g. once a minute from a watchdog task) — it only reads
internal state under the same short critical section everything else
uses.

```cpp
ntp_selftest_result_t r = ntp.selfTest();
if (!r.ok) {
    Serial.printf("[NTP selfTest] %s\n", r.message);
    // r.callbacksValid, r.precisionInRange, r.associationTableConsistent,
    // r.tokenBucketTableConsistent are individually inspectable too
}
```

Note: this validates internal wiring/invariants only — it has no
independent time reference, so it cannot tell you whether the time you're
serving is *accurate*, only that the server's own bookkeeping is
self-consistent.

---

## 6. Response Quality Filter (formerly "Clock Filter")

### 6.1. What changed in 3.5.0, and why

Prior versions labeled this feature "Clock Filter — RFC 5905 Appendix
A.5.1.1" and described it as computing "the client's clock offset". That
claim was wrong, and not fixable by patching the math: RFC 5905's clock
filter algorithm runs on the **client**, over samples of
`(T1, T2, T3, T4)` where **T4 is the time the client received the
server's reply** — a value the server never sees. Without T4, you cannot
compute a true peer offset; you can only compute what the server itself
observed, which is the gap between the client's claimed transmit time
(`T1`) and the server's own receive time (`T2`).

So this server passes `T4 = T3` (the server's own transmit time) into the
same shift-register/sort/dispersion/jitter/spike-detector math the RFC
uses, which collapses the round-trip delay term and leaves:

```
offset ≈ (T2 − T1) / 2        // server-observed, NOT the client's real offset
delay  ≈  T2 − T1             // one-way gap, not a round-trip delay
```

This is a real, useful **diagnostic** signal — it tracks how far the
client's clock plus network latency appears to be from the server's
clock, and the spike detector genuinely rejects sudden discontinuities.
What it is *not*: a substitute for the client computing its own offset
the normal NTP way from `T1..T4`. Every stock NTP client (`ntpd`,
`chrony`, Windows W32Time, etc.) already does this correctly on its own;
this server's filter output is for **your own monitoring/alerting**, not
for telling clients what their offset is.

The public API (`setClockFilterEnabled`, `getClockFilterState`,
`getFilterStats`, `clearClockFilter`, the `NTP_ENABLE_CLOCK_FILTER`
macro) is unchanged from 3.1.1 for compatibility with existing sketches.

### 6.2. What it produces
 - `peerOffset` (θ₀): smoothed server-observed one-way timing gap (see §6.1 — not a true client offset)
 - `peerDelay` (δ₀): lowest observed one-way gap among the last 8 samples
 - `peerDispersion` (ε): weighted dispersion average
 - `peerJitter` (ψ): RMS standard deviation of the offset samples

### 6.3. Internal Architecture
```
  ┌─────────────────────────────────────────┐
  │  Response Quality Filter per Client     │
  ├─────────────────────────────────────────┤
  │  8-Stage Shift Register                 │
  │  ┌───┬───┬───┬───┬───┬───┬───┬───┐      │
  │  │S0 │S1 │S2 │S3 │S4 │S5 │S6 │S7 │      │
  │  │new│old│   │   │   │   │   │   │      │
  │  └───┴───┴───┴───┴───┴───┴───┴───┘      │
  │  Each stage: (offset, delay, disp, t)   │
  ├─────────────────────────────────────────┤
  │  Sort by Delay → Select S0 (min delay)  │
  │  Weighted Dispersion: ε = Σ(εᵢ/2^(i+1)) │
  │  RMS Jitter: ψ = sqrt(Σ(θᵢ-θ₀)²/(n-1))  │
  │  Spike Detector: |Δθ| > 3×ψ ? reject    │
  └─────────────────────────────────────────┘
```

### 6.4. Algorithm Steps
| Step | Operation | Formula/Description |
| -------------------- | ------------------------------------------------ | ------------------------------- |
| 1. Shift             | New sample into S0, others shift left            | `S[i] = S[i-1]` (insertion-sort based, faster than the 3.1.1 bubble sort) |
| 2. Dispersion Growth | Old samples' dispersion grows at rate PHI        | `ε += Φ × Δt , Φ = 15 ppm` |
| 3. Sort              | Sort by ascending delay                          | Lowest delay = most trusted sample |
| 4. Select            | Select S0 (after sort) as peerOffset & peerDelay | `θ₀ = S[0].offset , δ₀ = S[0].delay` |
| 5. Dispersion        | Weighted average                                 | `ε = Σ(εᵢ / 2^(i+1))` |
| 6. Jitter            | RMS deviation from selected sample               | `ψ = sqrt(Σ(θᵢ - θ₀)² / (n-1))` |
| 7. Spike Detect      | If jump > 3× jitter, reject                      |  |

### 6.5. API
`void setClockFilterEnabled(bool enabled)`  
Enable/disable the filter. Enabled by default  

`bool getClockFilterState(int assocIndex, ntp_clock_filter_t& out) const`  
Retrieves the complete filter state for one client  

```cpp
ntp_clock_filter_t cf;
if (ntp.getClockFilterState(0, cf)) {
    Serial.printf("Offset: %.6f sec\n", cf.peerOffset);
    Serial.printf("Delay:  %.6f sec\n", cf.peerDelay);
    Serial.printf("Disp:   %.6f sec\n", cf.peerDispersion);
    Serial.printf("Jitter: %.6f sec\n", cf.peerJitter);
    Serial.printf("Valid:  %d/8 stages\n", cf.validStages);

    // Access individual stages
    for (int i = 0; i < NTP_CF_NSTAGE; i++) {
        Serial.printf("  S%d: off=%.6f del=%.6f disp=%.6f\n",
                      i, cf.stage[i].offset, cf.stage[i].delay, cf.stage[i].dispersion);
    }
}
```

`void getFilterStats(int assocIndex, double& offsetSec, double& delaySec, double& dispersion, double& jitterSec, uint8_t& validStages) const`  
Retrieves simplified filter outputs  

```cpp
double off, del, disp, jit;
uint8_t valid;
ntp.getFilterStats(0, off, del, disp, jit, valid);
Serial.printf("Filtered: off=%.1f us, delay=%.1f us, jitter=%.1f us\n",
              off * 1e6, del * 1e6, jit * 1e6);
```

`void clearClockFilter(int assocIndex)`  
Resets the filter shift register for one client to initial state

### 6.6. Sample Output
```text
--- Client Associations (with Response Quality Filter) ---

Client #1: 192.168.1.50
  Packets:     RX=45 TX=45
  Raw one-way estimate (server-observed, NOT true client offset): 1234 us
  Raw delay estimate:  2456 us
  
  --- Response Quality Filter ---
  Filtered Offset:  0.000123 sec (123.0 us)
  Filtered Delay:   0.002456 sec (2456.0 us)
  Dispersion:       0.000089 sec
  Jitter (RMS):     0.000045 sec (45.0 us)
  Valid Stages:     4/8
  Root Distance:    0.001311 sec
```

### 6.7. Disabling to Save RAM
If RAM is limited (e.g., ESP8266):

```cpp
// BEFORE including the header:
#define NTP_ENABLE_CLOCK_FILTER 0
#include "NTPServer_Advanced.h"
```

This frees approximately 256 bytes of RAM (32 associations × 8 stages × 1 byte + metadata)

### 6.8. If you need a real client-perceived offset
Compute it on the client, the standard way, from the four timestamps it
already has: `T1` (its own send time), `T2`/`T3` (echoed back by this
server in `rxTime`/`txTime`), and `T4` (its own receive time):

```
offset = ((T2 − T1) + (T3 − T4)) / 2
delay  = (T4 − T1) − (T3 − T2)
```

Every standard NTP client library already does this; you only need it
yourself if you're writing a custom client against this server.

---

## 7. Advanced Configuration
### 7.1. Compile-Time Macros
| Macro                     | Default | Description                                |
| ------------------------- | ------- | ------------------------------------------ |
| `NTP_LOG_LEVEL`           | `2`     | Log level: 0=Off to 4=Debug                |
| `NTP_MAX_ASSOCIATIONS`    | `32`    | Max clients / rate buckets / response quality filters |
| `NTP_MAX_LEAP_EVENTS`     | `8`     | Max leap second events                     |
| `NTP_AUTH_DIGEST_LEN`     | `16`    | Digest length (MD5=16)                     |
| `NTP_ENABLE_SYSLOG`       | `0`     | Enable Syslog forwarding                   |
| `NTP_ENABLE_CLOCK_FILTER` | `1`     | Enable Response Quality Filter             |
| `NTP_POLL_MIN`            | `4`     | Minimum echoed poll interval, log2 seconds (new in 3.5.0) |
| `NTP_POLL_MAX`            | `10`    | Maximum echoed poll interval, log2 seconds (new in 3.5.0) |

Setting in code:
```cpp
#define NTP_LOG_LEVEL 3
#define NTP_MAX_ASSOCIATIONS 64
#define NTP_ENABLE_CLOCK_FILTER 1
#include "NTPServer_Advanced.h"
```

### 7.2. Response Quality Filter Constants
| Constant         | Value      | Description                         |
| ---------------- | ---------- | ----------------------------------- |
| `NTP_CF_NSTAGE`  | `8`        | Shift register stages               |
| `NTP_CF_MAXDISP` | `16.0`     | Maximum dispersion (seconds)        |
| `NTP_CF_PHI`     | `0.000015` | Clock skew rate (15 ppm)            |
| `NTP_CF_SGATE`   | `3.0`      | Spike detector threshold (× jitter) |

### 7.3. Performance Recommendations
| Parameter              | Recommended Industrial Value        |
| ---------------------- | ----------------------------------- |
| `rateLimitRate`        | 1 to 2 requests/second              |
| `rateLimitBurst`       | 3 to 5                              |
| `multicastIntervalSec` | 64 (2^6) or higher                  |
| `rootDispersion`       | 10× rootDelay                       |
| `precision`            | Auto-measure (`measurePrecision()`) |

---

## 8. Security
### 8.1. Threats & Countermeasures
| Threat              | Countermeasure                              |
| ------------------- | ------------------------------------------- |
| NTP Amplification   | Token Bucket Rate Limiting + KoD(RATE)      |
| Spoofing (payload tampering) | Authentication MAC (Symmetric Key), constant-time compared; KoD(AUTH) on failure |
| Malformed/hostile extension fields | Extension-field walker bails out safely on an invalid length instead of reading out of bounds; responds KoD(CRYP) |
| Reconnaissance      | Limited Association Table + no full monlist |
| Resource Exhaustion | Fixed-capacity tables + LRU eviction        |
| Time Attack         | Leap Second validation + LI integrity       |
| Spike/Noise         | Response Quality Filter + SGATE rejection   |

### 8.2. Known limitation: source-IP spoofing

Rate limiting and the association table are both keyed by the UDP source
IP in the received packet. UDP has no handshake, so an attacker can send
packets with forged source IPs and consume slots in either 32-entry table
under a flood of fake addresses, evicting real clients (LRU eviction
means a large-enough spoofed flood can churn through the table faster
than legitimate clients get re-added). This is **not unique to this
library** — it's inherent to any UDP-based, source-IP-keyed rate limiter,
including stock `ntpd`. Mitigations that are outside library scope but
worth doing at the network layer: ingress filtering (BCP 38) on your
router/switch to drop obviously-spoofed source addresses, and/or putting
this server behind a network that clients can only reach if their own
source IP is enforced (e.g., a wired industrial VLAN with port security).
Authentication (MAC) does not prevent table exhaustion either, since
`_checkTokenBucket` and `_findOrCreateAssociation` both run before the
authentication check in the packet path.

### 8.3. Best Practices
1. Always keep Rate Limiting enabled, even on internal networks
2. Use Authentication on untrusted networks — and after upgrading to
   3.5.0, verify with a real client that auth actually rejects a bad MAC
   (see §5.4)
3. Store keys in Secure Element or Flash Encrypted
4. Set Stratum correctly. An unsynced server must not claim stratum 1-15
5. Set Root Dispersion realistically. A value lower than reality causes clients to incorrectly trust you
6. Keep the Response Quality Filter enabled in noisy environments for its spike-rejection diagnostics — but don't rely on its `peerOffset` as a substitute for the client computing its own offset (§6.1)
7. If deploying on a network you don't fully control, add ingress/source-IP filtering upstream (§8.2) — this library's rate limiting alone cannot stop a spoofed-source flood

---

## 9. Usage Examples
### 9.1. GPS Primary Server (Stratum 1) with Response Quality Filter
```cpp
AdvancedNTPServer ntp;

uint32_t getUTC() { return gps.time.value(); }  // TinyGPS++
uint32_t getUS()  { return gps.time.age() < 1000 ? 0 : 500000; }

void setup() {
    ntp.begin(123, getUTC, getUS);
    ntp.setMode(NTPMode_t::GPS);
    ntp.setRateLimitParams(2, 5);
    ntp.measurePrecision();

    // Response Quality Filter enabled by default
    ntp.setClockFilterEnabled(true);
}

void loop() {
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 30000) {
        lastPrint = millis();

        ntp_association_t assoc[4];
        size_t count;
        ntp.getAssociations(assoc, 4, count);

        for (size_t i = 0; i < count; i++) {
            Serial.printf("Client %s: jitter=%.1f us, valid=%d/8\n",
                          IPAddress(assoc[i].ip).toString().c_str(),
                          assoc[i].filteredJitter * 1e6,
                          assoc[i].filterValid);
        }
    }
}
```

### 9.2. Secondary Server with Auth and Response Quality Filter
```cpp
ntp.begin(123, getUTC, getUS);
ntp.setMode(NTPMode_t::SYNC);
ntp.setAuthEnabled(true);
ntp.setAuthCallback([](uint32_t keyId, const uint8_t* d, size_t l, uint8_t* out) {
    // Real MAC implementation required here — see §5.4.
    // (This stub always returns true, which authenticates every key;
    // don't ship it as-is.)
    return true;
});

// Monitor response quality (diagnostic — see §6.1, not a true client offset)
double offset, delay, disp, jitter;
uint8_t valid;
ntp.getFilterStats(0, offset, delay, disp, jitter, valid);
if (jitter > 0.001) {  // > 1ms jitter is suspicious
    Serial.println("Warning: High jitter detected!");
}
```

### 9.3. Remote Monitoring
```cpp
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 30000) {
        last = millis();
        auto s = ntp.getStatistics();
        Serial.printf("RX=%u TX=%u Drop=%u Drift=%.2f ppm\n",
                      s.packetsReceived, s.packetsSent,
                      s.packetsDropped, s.estimatedDriftPPM);
    }
}
```

---

## 10. Limitations & Performance Considerations
| Limitation     | Description                                                                |
| -------------- | -------------------------------------------------------------------------- |
| RAM            | Roughly 2.2 KB per server instance without the Response Quality Filter and 13 KB with it (32 associations × 8 stages; measured on a 64-bit host, a 32-bit target is slightly smaller). Lower `NTP_MAX_ASSOCIATIONS` or set `NTP_ENABLE_CLOCK_FILTER 0` on ESP8266 |
| CPU            | Not benchmarked. Critical sections cover table operations only, never packet I/O. On ESP8266 they use `noInterrupts()`, and the filter math uses software floating point, so keep `NTP_ENABLE_CLOCK_FILTER` off if you see WiFi instability |
| Accuracy       | Limited by `micros()` precision and WiFi jitter (typically ±1 ms)          | 
| Authentication | MD5 only by default. NTS (TLS) not supported (insufficient RAM/Flash). Swap in HMAC-SHA256 in your own callback if you don't need legacy interop | 
| Peers          | Server/Broadcast only. Full Symmetric Active/Passive not implemented (mode-1 requests are answered as a server) | 
| Drift estimate | `estimatedDriftPPM` compares the UTC callbacks with `millis()` over ≥60 s windows. It is only meaningful if your time source is independent of `millis()`; a step correction inside the window shows as a one-off spike |
| Year 2036 / 2106 | Safe across the NTP era rollover on 2036-02-07: wire timestamps wrap exactly as RFC 5905 specifies (clients resolve the era themselves), offset/delay math is wrap-safe, and the leap table works in Unix seconds. Your `getUTC` callback returns a 32-bit Unix time, which lasts until 2106. Beware that on ESP8266 the SDK's own `time_t` is 32-bit *signed* and overflows in 2038, so do not feed it `gettimeofday()` past then |
| Broadcast      | Multicast works on WiFi STA/AP only. Mesh has limitations                  | 
| Response Quality Filter | Diagnostic only — does **not** compute a true RFC 5905 client offset (server never sees T4); see §6.1. Clustering (A.5.1.2) is not implemented (not applicable server-side) |
| Temperature Compensation | Linear PPM model, actually applied to outgoing timestamps as of 4.0.0 (§5, "Temperature Compensation"). Accuracy depends entirely on how well `ppmPerC` characterizes your actual crystal/RTC — it is not a substitute for a properly temperature-compensated oscillator (TCXO/OCXO) if you need tight accuracy |
| Extension Fields | The TLV walker (§5.4, §8) locates the legacy MAC behind unrecognized extension fields, but does not parse or act on the *content* of any extension field type other than the legacy MAC — e.g. no NTS support |
| IPv6 | Not supported. This is a genuine architectural gap, not an oversight: association records, the token-bucket table, and the packet-processing path all key on a `uint32_t` IPv4 address end-to-end. Adding IPv6 properly means widening every one of those (association/bucket storage, `remoteIP()` handling, `getAssociations()`'s public struct shape) rather than a local patch, so it's left as a clearly-scoped future major version rather than attempted as a partial/inconsistent add-on here |
| Source-IP spoofing | Rate limiting and the association table are keyed by source IP, which is trivially spoofable over UDP; see §8.2 |
| Concurrency    | Shared state is now protected by a critical section (§3.4), but the sections are still coarse enough that very high packet rates on a dual-core ESP32 will serialize somewhat between the UDP task and your `loop()` calls to the getters |

---

## 11. Troubleshooting
| Symptom                     | Likely Cause      | Solution           |
| --------------------------- | ----------------- | ------------------ |
| Client `no server suitable` | Stratum=0 or LI=3 | Check `setMode()` configuration |
| Very large `offset`         | Wrong `NTP_TIMESTAMP_DELTA` | Ensure UTC callback is correct |
| Zero or negative `delay`    | `getSubSec` is not μs       | Value must be 0-999999, not 0-999 |
| Auth always failing after upgrading to 3.5.0+ | You were relying on the 3.1.1 bug (auth silently never verified) | Fix your client's MAC/KeyID — the fixed check is the correct one |
| Repeated Auth failures (new deployment) | Wrong KeyID or Secret | Debug auth callback with Wireshark/hex; confirm KeyID is 4 bytes network-order at the start of the extension field |
| Client receives KoD(CRYP)   | Extension field(s) present but no valid legacy MAC located after them | Confirm your client isn't sending a malformed/oversized extension field; see §5.4 |
| Compile error: `setPeerTxTimestamp` not found | Upgraded to 4.0.0 with old sketch code | Delete the call — Interleaved Mode tracks each client's T3 automatically now; see §5.9 |
| Unexpected rate limiting    | Clock drift in `millis()`   | Call `clearRateLimitTable()` or increase `burst` |
| Crash in `begin()`          | Null callback               | Always verify `getUTC` and `getSubSec` |
| Very high jitter            | Noisy network or overload   | Check the Response Quality Filter; spike detector should filter noise |
| Repeated spike detected     | Client clock is jumping     | Investigate client. May have CPU overload |
| Valid stages = 0            | Only one packet received    | Minimum 2 packets needed for the filter. Wait |
| `peerOffset` doesn't match my client's own offset reading | Expected — see §6.1/§6.8 | This value is a server-side diagnostic, not the client's true offset; compute the client's real offset on the client from its own T1..T4 |

---

## 12. RFC 5905 Compliance Matrix
| RFC Section               | Status           | Notes  |
| ------------------------- | ---------------- | --------------------------------------- |
| 7.3 Packet Format         | ✅ Complete      | 48-byte packed, correct bit-fields |
| 7.3 Timestamps            | ✅ Complete      | T1/T2/T3/T4 fields populated correctly | 
| 7.3 Root Delay/Dispersion | ✅ Complete      | Short 16.16 format |
| 7.4 Kiss-o'-Death         | ✅ Complete      | Stratum=0 + ASCII RefID, origin timestamp echoed so clients accept it (4.1.0) |
| 7.5 NTP Control Messages  | ❌ Not implemented | Mode-6 packets are counted (`controlMsgsReceived`) and dropped without a reply |
| 7.2 Poll Interval         | ✅               | Server-side clamp to configured bounds, was previously echoed unclamped |
| RFC 7822 Extension Fields    | ⚠️ Partial       | Generic TLV walker locates the legacy MAC behind unrecognized fields (4.0.0); does not parse/act on other field types (e.g. no NTS) |
| 8. Authentication         | ⚠️ Symmetric Key | MD5 MAC by default, constant-time compare. Auth verification itself is functionally correct (fixed in 3.5.0 — was broken in 3.1.1). Autokey/NTS not supported |
| 9. System State           | ✅               | leap, stratum, precision, refTime | 
| A.5.1.1 Clock Filter      | ❌ Not implemented as specified | This server implements a same-shaped diagnostic filter (§6) using math from A.5.1.1/A.5.2, but it cannot be RFC-compliant: A.5.1.1 requires the client's T4, which a server structurally never receives. Previous docs incorrectly claimed full compliance here |
| A.5.1.2 Clustering        | ❌               | Not implemented (algorithm applies to a client selecting among multiple servers; not applicable here) |
| A.5.2.1 Root Distance     | ⚠️ Approximate   | `lambda = delta/2 + epsilon + PHI*(t-t0)`, computed from the same server-side diagnostic values as §6, not a true root distance |
| A.5.2.2 fit()             | ⚠️ Approximate   | Basic implementation, same caveat as above |
| 15. Security              | ✅               | Rate limit + KoD + Auth (now functionally correct) + constant-time MAC compare |

---

## 13. Changelog
| Version | Date | Changes |
| ------- | ---- | ------- |
| 1.0.0 | — | Initial release (NTPServer_SHN) |
| 2.0.0 | — | RFC bug fixes, Rate Limiting, Statistics |
| 3.0.0 | — | Auth, Multicast, Token Bucket, Drift, Temp Comp, Interleaved, Syslog |
| 3.1.1 | — | "Clock Filter" (RFC 5905 A.5.1.1-derived math): 8-stage shift register, spike detector, per-client jitter/dispersion |
| 3.5.0 | 2026 | Bug-fix/hardening release: fixed non-functional authentication (Key-ID was read from a pointer value, not packet bytes), fixed a sign-loss bug in the offset/delay fixed-point math, added a critical section around shared state, made MAC comparison constant-time, added `setPollLimits()`, and relabeled "Clock Filter" to "Response Quality Filter" in documentation to stop claiming RFC 5905 A.5.1.1 compliance a server structurally cannot have (no T4) |
| **4.0.0** | **2026** | **Architecture-hardening release:** ① **[Breaking]** Removed `setPeerTxTimestamp(const ntp_timestamp_t&)` — Interleaved Mode's T3 is now tracked automatically per client in `ntp_association_t::peerTxTimestamp`, fixing state corruption when multiple clients used Interleaved Mode concurrently against the old single shared timestamp. ② **[Fix]** The extension-field parser now walks RFC 7822 TLV fields to locate the trailing legacy MAC, instead of assuming the MAC is the only thing after the 48-byte header — a client sending any extension field before its MAC used to have that MAC misread. Malformed field lengths are rejected safely (no out-of-bounds read) with `KoD(CRYP)`. ③ **[Fix]** Temperature compensation now actually corrects outgoing timestamps (linear PPM model with proper second/microsecond carry, extracted into the independently-testable `_foldMicroseconds()`), instead of computing a value that was discarded. ④ **[New]** Additional Kiss-o'-Death codes — `AUTH` for authentication failure (previously the generic `DENY`), `CRYP` for extension-field parsing failure, `RSTR` reserved for future access-policy use — alongside the existing `RATE`. ⑤ **[New]** `selfTest()` for a lightweight, network-free runtime consistency check suitable for periodic watchdog use. ⑥ **[New]** `NTP_SERVER_VERSION_MAJOR/MINOR/PATCH/STRING` macros. ⑦ **[New]** `library.json` and `library.properties` for PlatformIO Registry / Arduino Library Manager publishing. ⑧ **[New]** Native, host-buildable unit test suite (`tests/test_ntp.cpp`, 48 assertions) plus a GitHub Actions workflow (`.github/workflows/tests.yml`) that runs it and sanity-compiles every `#if` configuration and the example sketch on every push/PR. ⑨ **[Docs]** IPv6 support is explicitly scoped out as a future major-version change, with the reason documented (§10) rather than left as a silent gap |
| **4.1.0** | **2026** | **Correctness release.** ① **[Fix]** MAC verification now hashes the packet as received instead of the host-byte-order copy, so real clients can authenticate. ② **[Fix]** Replies to authenticated requests are signed with the client's key. ③ **[Fix]** `refId` is no longer byte-swapped (affected `GPS`, `LCLS` and every KoD code). ④ **[Fix]** KoD packets echo the client's transmit timestamp and version. ⑤ **[Fix]** KoD/replies are written outside the critical section (writing with interrupts disabled could crash or deadlock on ESP32). ⑥ **[Fix]** The `getLeap` callback and leap-event table now actually set LI. ⑦ **[Fix]** Drift estimate used mismatched windows and units. ⑧ **[Hardening]** `importConfig()` range-checks all fields. ⑨ **[Packaging]** Standard `src/`+`examples/` layout, repaired `library.json`/`library.properties`, working CI paths, keywords, `.gitignore`. ⑩ **[Fix]** Year-2036 safety: leap-event table stored/compared in 64-bit Unix seconds (the old 32-bit NTP-seconds comparison failed within a day of the 2036-02-07 rollover), and offset/delay arithmetic no longer produces ~4.29e9 s errors across the rollover ⑪ **[Removed]** The simplified mode-6 reply (it did not follow the control-message format, so `ntpq` could not use it); mode-6 packets are now counted and dropped ⑫ **[Tests]** 99 assertions including end-to-end packet and era-rollover tests |
| **4.0.1** | **2026** | **Compile-fix patch, reported by a real ESP32/Arduino-IDE build:** ① **[Fix]** `NTP_LOCK_INIT` used `PORTMUX_INITIALIZER_UNLOCKED`, which does not exist in the real Arduino-ESP32 core — the actual macro is `portMUX_INITIALIZER_UNLOCKED` (lowercase p). This broke every ESP32 compile. It slipped through 4.0.0's sanity checks because the test/CI stub header (`tests/stubs/esp32-hal.h`) had copied the same wrong name, so the mistake was internally consistent but wrong relative to the real SDK — the stub is now corrected to match the real casing so this class of mismatch would be caught going forward. ② **[Docs]** Added §4.0 ("Folder Layout — Read This First") explaining that Arduino compiles every `.cpp` in the sketch folder, so `tests/test_ntp.cpp` must be kept out of it (it needs `NTP_TESTING` defined, which the Arduino build never sets, otherwise every private-member access in the test file fails to compile). Added a matching warning comment at the top of `test_ntp.cpp` itself as a second line of defense |

---

## 14. Testing & CI

### 14.1. Running the tests locally
No ESP32/ESP8266 toolchain is required — the test suite builds against
lightweight Arduino/AsyncUDP stub headers with the system `g++`:

```bash
cd tests
make run
```

Expected output ends with e.g. `99 passed, 0 failed`, and the process
exits `0` iff everything passed.

### 14.2. What's covered
- Fixed-point timestamp math (`_usToFraction`, `_secondsToShort`,
  round-trips) and the exact sign-loss bug fixed in 3.5.0
  (`_estimateOffset`/`_estimateDelay` with a negative fraction difference)
- Constant-time MAC comparison, including a difference at the very first
  byte (a naive early-exit `memcmp` would still be "safe" here by luck —
  this specifically checks the *whole* array is compared)
- The RFC 7822 extension-field walker: a bare legacy MAC, a MAC preceded
  by a TLV field, and a deliberately malformed field length (must fail
  closed, not read out of bounds)
- The microsecond-folding carry logic in both directions, including the
  epoch-clamp edge case
- Token bucket admission/denial and per-IP isolation
- Association table lookup/creation
- Poll-interval limit setting and rejection of an inverted range
- `selfTest()` before and after `begin()`
- Whole-packet behavior through the real `_processPacket()`: reply fields and origin-timestamp echo, rate-limit KoD, authentication (wire-byte digest, signed replies, bad/missing MAC), leap indicator (table, callback, precedence), drift estimate, `importConfig()` sanitising

### 14.3. How it reaches private internals
A `friend class NTPServerTestAccess`, compiled in only when `NTP_TESTING`
is defined (never in a normal library build), exposes the private static
helpers and a few private instance methods directly to the test file.
This avoids both `#define private public` (undefined behavior in
standard C++, and fragile against reordering) and duplicating logic in a
parallel "testable" copy that could drift from the real implementation.

### 14.4. CI
`.github/workflows/tests.yml` runs on every push and pull request that
touches the library or the tests: it builds and runs the native test
suite, checks that the version in the header, `library.json` and `library.properties` agree, then sanity-compiles the library against `g++` in all
four `#if` configurations (ESP32, ESP8266, Response Quality Filter
disabled, Syslog enabled) and compiles the example sketch — the same
checks performed by hand during this release, now automated so a future
change can't silently reintroduce a per-configuration compile break.

---

### License
MIT — see [LICENSE](LICENSE). The software is provided "as is"; use in critical and industrial systems is the sole responsibility of the user.

---

*Document Version: 4.1.0*  
*Last Updated: 2026*