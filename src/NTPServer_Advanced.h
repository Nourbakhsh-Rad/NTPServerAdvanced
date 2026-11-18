//-----------------------------------------------------------------------------
// Industrial NTP Server v4.1.0 — Advanced Edition + Response Quality Filter
// RFC 5905 (NTPv4) + Authentication + Extension Fields + Diagnostic Filter
// Platforms: ESP32 / ESP8266
//
// v4.1.0 changelog highlights (see README §13 for the full list):
//   - FIX: MAC verification hashed the packet *after* it had been converted
//     to host byte order, so no real client could ever authenticate. It now
//     hashes the bytes exactly as received.
//   - FIX: replies to authenticated clients are now signed (KeyID + digest),
//     which keyed clients (ntpd/chrony) require before they accept a reply.
//   - FIX: refId (e.g. "GPS", "RATE") was byte-swapped on the wire.
//   - FIX: Kiss-o'-Death packets now echo the client's transmit timestamp
//     and version, otherwise RFC 5905 clients discard them as bogus.
//   - FIX: KoD / reply sending no longer happens inside a critical section.
//   - FIX: the leap callback and leap-event table were never applied to the
//     LI field; they are now (callback wins, table is the fallback).
//   - FIX: drift estimate compared mismatched time windows and mixed units.
//   - FIX: year-2036 (NTP era 1) safety: leap table is kept in Unix seconds and
//     offset/delay arithmetic is wrap-safe across the 2036-02-07 rollover.
//   - REMOVED: the non-conforming mode-6 reply. Control messages are now
//     counted (controlMsgsReceived) and silently dropped.
//   - HARDENING: importConfig() range-checks every field.
//   - TESTS: whole-packet tests drive _processPacket() end to end.
//
// v4.0.1 changelog highlights (see README §13 for full list; §13 also has
// the full v3.5.0/4.0.0 history):
//   - FIX: NTP_LOCK_INIT used the wrong macro name for the ESP32 spinlock
//     initializer (PORTMUX_INITIALIZER_UNLOCKED instead of the real
//     portMUX_INITIALIZER_UNLOCKED, lowercase p), which failed to compile
//     against the real esp32-hal.h. The test/CI stub headers had the same
//     wrong name, so this was never caught by the sanity compiles — fixed
//     in both places, and the stub now matches the real Arduino-ESP32
//     core spelling.
//   - DOCS: clarified that tests/ must not be placed inside an Arduino
//     sketch folder (the IDE compiles every .cpp it finds there,
//     including test_ntp.cpp, which needs NTP_TESTING to build). Obsolete
//     since 4.1.0: tests/ now sits outside src/ and is never compiled.
//
// v4.0.0 changelog highlights:
//   - BREAKING: setPeerTxTimestamp(const ntp_timestamp_t&) is replaced by
//     per-client, fully-automatic Interleaved Mode bookkeeping. The server
//     now tracks each client's previous T3 in its own association record;
//     you no longer call this manually. See README §5.9.
//   - NEW: a real RFC 7822-aware extension-field walker locates the
//     trailing legacy MAC even when preceded by TLV extension fields the
//     server doesn't otherwise understand, instead of assuming the MAC is
//     always the only thing after the 48-byte header.
//   - NEW: temperature compensation now actually adjusts outgoing
//     timestamps (linear PPM model), instead of only computing a value
//     that was discarded.
//   - NEW: additional standard Kiss-o'-Death codes (AUTH, CRYP, RSTR)
//     alongside RATE, so clients can distinguish rate-limiting from auth
//     failure from other rejections.
//   - NEW: selfTest() for a lightweight runtime health/consistency check.
//   - NEW: NTP_SERVER_VERSION_* macros.
//   - NEW: library.json / library.properties for PlatformIO / Arduino
//     Library Manager publishing.
//   - NEW: native unit test suite (tests/ directory) covering the fixed-
//     point math, token bucket, association table, extension-field
//     walker, and constant-time comparison.
//-----------------------------------------------------------------------------

#ifndef _NTP_SERVER_ADVANCED_H_
#define _NTP_SERVER_ADVANCED_H_

#include <Arduino.h>
#include <lwip/def.h>
#include <math.h>
#include <functional>

#ifdef ESP32
#include <AsyncUDP.h>
#include <esp32-hal.h>
#else
#include <ESPAsyncUDP.h>
#endif

// ---------------------------------------------------------------------------
// Library Version
// ---------------------------------------------------------------------------
#define NTP_SERVER_VERSION_MAJOR 4
#define NTP_SERVER_VERSION_MINOR 1
#define NTP_SERVER_VERSION_PATCH 0
#define NTP_SERVER_VERSION_STRING "4.1.0"

// ---------------------------------------------------------------------------
// Compile-Time Configuration
// ---------------------------------------------------------------------------
#ifndef NTP_LOG_LEVEL
#define NTP_LOG_LEVEL 2
#endif

#ifndef NTP_MAX_ASSOCIATIONS
#define NTP_MAX_ASSOCIATIONS 32
#endif

#ifndef NTP_MAX_LEAP_EVENTS
#define NTP_MAX_LEAP_EVENTS 8
#endif

#ifndef NTP_AUTH_DIGEST_LEN
#define NTP_AUTH_DIGEST_LEN 16
#endif

#ifndef NTP_ENABLE_SYSLOG
#define NTP_ENABLE_SYSLOG 0
#endif

// Kept as NTP_ENABLE_CLOCK_FILTER for backward compatibility with existing
// sketches / build flags. It now guards the "Response Quality Filter".
#ifndef NTP_ENABLE_CLOCK_FILTER
#define NTP_ENABLE_CLOCK_FILTER 1
#endif

// ---------------------------------------------------------------------------
// Logging Macros
// ---------------------------------------------------------------------------
#if NTP_LOG_LEVEL >= 1
#define NTP_LOGE(fmt, ...) Serial.printf("[NTPs][E] " fmt "\r\n", ##__VA_ARGS__)
#else
#define NTP_LOGE(...)
#endif

#if NTP_LOG_LEVEL >= 2
#define NTP_LOGW(fmt, ...) Serial.printf("[NTPs][W] " fmt "\r\n", ##__VA_ARGS__)
#else
#define NTP_LOGW(...)
#endif

#if NTP_LOG_LEVEL >= 3
#define NTP_LOGI(fmt, ...) Serial.printf("[NTPs][I] " fmt "\r\n", ##__VA_ARGS__)
#else
#define NTP_LOGI(...)
#endif

#if NTP_LOG_LEVEL >= 4
#define NTP_LOGD(fmt, ...) Serial.printf("[NTPs][D] " fmt "\r\n", ##__VA_ARGS__)
#else
#define NTP_LOGD(...)
#endif

// ---------------------------------------------------------------------------
// Constants (RFC 5905)
// ---------------------------------------------------------------------------
#define NTP_SRV_DEFAULT_PORT 123
#define NTP_TIMESTAMP_DELTA 2208988800ull

#define NTP_SHORT_SCALE 65536.0
#define NTP_STRATUM_UNSPECIFIED 0
#define NTP_STRATUM_PRIMARY 1
#define NTP_STRATUM_MAX 15
#define NTP_STRATUM_UNSYNC 16

#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4
#define NTP_MODE_BROADCAST 5
#define NTP_MODE_CONTROL 6

#define NTP_LI_NO_WARNING 0
#define NTP_LI_ADD_SECOND 1
#define NTP_LI_DEL_SECOND 2
#define NTP_LI_NOT_IN_SYNC 3

// Kiss-o'-Death codes (RFC 5905 §7.4 / IANA "NTP Kiss-o'-Death Codes").
// Using the specific code lets a well-behaved client react appropriately
// (e.g. back off polling on RATE, stop retrying the same key on AUTH)
// instead of getting a generic rejection.
#define NTP_KOD_RATE "RATE" // client is exceeding the rate limit — poll slower
#define NTP_KOD_AUTH "AUTH" // authentication failed (bad MAC / unknown key)
#define NTP_KOD_CRYP "CRYP" // cryptographic/extension-field parsing failure
#define NTP_KOD_RSTR "RSTR" // access restricted by server policy
#define NTP_KOD_DENY "DENY" // generic denial (fallback)

// Server poll-interval bounds (RFC 5905 §7.2): the server should not blindly
// echo back whatever poll value the client sends; it should clamp it to a
// sane operating range before including it in the response.
#ifndef NTP_POLL_MIN
#define NTP_POLL_MIN 4 // 16s
#endif
#ifndef NTP_POLL_MAX
#define NTP_POLL_MAX 10 // 1024s
#endif

// Response Quality Filter constants (formerly "Clock Filter" constants).
// Math is derived from RFC 5905 A.5.1.1 / A.5.2, reused here as a
// per-client smoothing/spike-detection diagnostic — see README §6.
#define NTP_CF_NSTAGE 8
#define NTP_CF_MAXDISP 16.0
#define NTP_CF_PHI 0.000015
#define NTP_CF_SGATE 3.0
#define NTP_CF_MAXPOLL 17
#define NTP_CF_MINPOLL 3

// ---------------------------------------------------------------------------
// Packed Structures
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t mode : 3;
    uint8_t version : 3;
    uint8_t li : 2;
}
ntp_flags_t;

typedef union __attribute__((packed)) {
    uint32_t u32;
    uint8_t u8[4];
    char str[4];
}
ntp_refid_t;

typedef struct __attribute__((packed)) {
    uint32_t seconds;
    uint32_t fraction;
}
ntp_timestamp_t;

typedef union __attribute__((packed)) {
    uint32_t raw;
    struct __attribute__((packed)) {
        uint16_t fraction;
        uint16_t seconds;
    };
}
ntp_short_t;

typedef struct __attribute__((packed)) {
    ntp_flags_t flags;
    uint8_t stratum;
    int8_t pollInterval;
    int8_t precision;
    ntp_short_t rootDelay;
    ntp_short_t rootDispersion;
    ntp_refid_t refId;
    ntp_timestamp_t refTime;
    ntp_timestamp_t origTime;
    ntp_timestamp_t rxTime;
    ntp_timestamp_t txTime;
}
ntp_packet_t;

static_assert(sizeof(ntp_packet_t) == 48, "NTP packet must be 48 bytes");

// ---------------------------------------------------------------------------
// Extension Field support (RFC 7822)
// ---------------------------------------------------------------------------
// A generic RFC 7822 extension field is [type:2][length:2][value:length-4],
// length-padded to a 4-byte boundary. The *legacy* symmetric-key MAC (what
// this library authenticates with) predates RFC 7822 and has no type/length
// header — it is simply [KeyID:4][Digest:N] placed after everything else.
// A client that also sends modern-style extension fields (e.g. future NTS
// negotiation fields) will place them *before* the legacy MAC, so the
// server must walk past any TLV fields it doesn't recognize to find the
// MAC at the tail instead of assuming the MAC is the only thing present.
typedef struct {
    uint16_t type;
    uint16_t length; // total field length incl. type+length, per RFC 7822
    const uint8_t *value;
} ntp_ext_field_t;

// ---------------------------------------------------------------------------
// Response Quality Filter Structures
// (formerly "Clock Filter", see README §6 for what this measures)
// ---------------------------------------------------------------------------
typedef struct {
    double offset;
    double delay;
    double dispersion;
    double time;
} ntp_cf_stage_t;

typedef struct {
    ntp_cf_stage_t stage[NTP_CF_NSTAGE];
    double peerOffset;
    double peerDelay;
    double peerDispersion;
    double peerJitter;
    double lastUpdateTime;
    uint8_t validStages;
} ntp_clock_filter_t;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
typedef enum class NTPMode : uint8_t { ALARM = 0, VALID = 1, SYNC = 2, GPS = 3 } NTPMode_t;

typedef enum class ClockQuality : uint8_t {
    UNKNOWN = 0,
    UNSTABLE = 1,
    WARMUP = 2,
    GOOD = 3,
    EXCELLENT = 4
} ClockQuality_t;

// ---------------------------------------------------------------------------
// Association Record (per client)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t ip;
    uint32_t lastSeenMs;
    uint32_t packetsReceived;
    uint32_t packetsSent;
    int32_t lastOffsetUs;
    int32_t lastDelayUs;
    uint8_t version;
    uint8_t mode;
    int8_t lastPoll;
    bool active;

    // Per-client Interleaved Mode state (v4.0.0). Holds this client's
    // previous response txTime (T3), which — per RFC 5905's interleaved
    // scheme — is echoed as origTime in the *next* response to the same
    // client instead of that client's own txTime. Automatically
    // maintained by the server; no manual bookkeeping needed. Zero means
    // "no prior sample yet", so the first response to a new client always
    // uses ordinary (non-interleaved) origTime regardless of this flag.
    ntp_timestamp_t peerTxTimestamp;

#if NTP_ENABLE_CLOCK_FILTER
    double filteredOffsetSec;
    double filteredDelaySec;
    double filteredDispersion;
    double filteredJitter;
    uint8_t filterValid;
#endif
} ntp_association_t;

// ---------------------------------------------------------------------------
// Statistics & Health
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t packetsReceived;
    uint32_t packetsSent;
    uint32_t packetsDropped;
    uint32_t packetsInvalid;
    uint32_t packetsAuthFail;
    uint32_t kissOfDeathSent;
    uint32_t controlMsgsReceived;
    uint32_t broadcastSent;
    uint32_t lastClientIP;
    uint32_t uptimeSeconds;
    float estimatedDriftPPM;
    ClockQuality_t clockQuality;
} ntp_statistics_t;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t version;
    uint8_t stratum;
    uint8_t li;
    char refId[4];
    int8_t precision;
    double rootDelaySec;
    double rootDispersionSec;
    uint32_t rateLimitBurst;
    uint32_t rateLimitRate;
    bool authEnabled;
    bool multicastEnabled;
    uint32_t multicastIntervalSec;
    bool temperatureCompEnabled;
    bool clockFilterEnabled;
} ntp_config_t;

// ---------------------------------------------------------------------------
// Callback Types
// ---------------------------------------------------------------------------
using ntp_utc_cb_t = std::function<uint32_t(void)>;
using ntp_subsec_cb_t = std::function<uint32_t(void)>;
using ntp_leap_cb_t = std::function<uint8_t(void)>;
using ntp_temp_cb_t = std::function<float(void)>;
using ntp_auth_cb_t = std::function<bool(uint32_t keyId, const uint8_t *data, size_t len, uint8_t *outDigest)>;

#if NTP_ENABLE_SYSLOG
using ntp_syslog_cb_t = std::function<void(const char *msg)>;
#endif

// ---------------------------------------------------------------------------
// Token Bucket
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t ip;
    uint32_t lastUpdateMs;
    float tokens;
    uint32_t violations;
    bool active;
    bool kodSent;
} ntp_token_bucket_t;

// ---------------------------------------------------------------------------
// Leap Event
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t unixTimestamp; // Unix seconds (valid until 2106 in 32 bits)
    uint8_t type;
    bool active;
} ntp_leap_event_t;

// ---------------------------------------------------------------------------
// Self-Test Result
// ---------------------------------------------------------------------------
typedef struct {
    bool ok;
    bool callbacksValid;
    bool precisionInRange;
    bool associationTableConsistent;
    bool tokenBucketTableConsistent;
    char message[64];
} ntp_selftest_result_t;

// ---------------------------------------------------------------------------
// Lightweight cross-platform critical section
// ---------------------------------------------------------------------------
// The UDP receive callback (AsyncUDP/ESPAsyncUDP) executes on the network
// stack's own task, not on the task that called begin()/loop(). Public
// getters (getStatistics, getAssociations, getFilterStats, ...) are
// typically called from the sketch's loop() task. Both sides touch the
// same tables, so all accesses are wrapped in a short critical section.
#ifdef ESP32
class NTPCriticalSection {
  public:
    explicit NTPCriticalSection(portMUX_TYPE *mux) : _mux(mux) { portENTER_CRITICAL(_mux); }
    ~NTPCriticalSection() { portEXIT_CRITICAL(_mux); }

  private:
    portMUX_TYPE *_mux;
};
#define NTP_LOCK_T portMUX_TYPE
#define NTP_LOCK_INIT portMUX_INITIALIZER_UNLOCKED
#define NTP_GUARD(mux) NTPCriticalSection _ntpGuard(&(mux))
#else
// ESP8266 is single-core; ESPAsyncUDP still runs its callback outside the
// sketch's loop() context, but noInterrupts()/interrupts() is sufficient
// to keep short table updates atomic with respect to it.
class NTPCriticalSection {
  public:
    NTPCriticalSection() { noInterrupts(); }
    ~NTPCriticalSection() { interrupts(); }
};
#define NTP_LOCK_T uint8_t
#define NTP_LOCK_INIT 0
#define NTP_GUARD(mux) NTPCriticalSection _ntpGuard{}
#endif

// ---------------------------------------------------------------------------
// Main Class
// ---------------------------------------------------------------------------
class AdvancedNTPServer {
  public:
    AdvancedNTPServer();
    ~AdvancedNTPServer();

    // Lifecycle
    bool begin(uint16_t port, ntp_utc_cb_t getUTC, ntp_subsec_cb_t getSubSec, ntp_leap_cb_t getLeap = nullptr,
               ntp_temp_cb_t getTemp = nullptr);
    void end();
    bool isRunning() const;

    // Control
    void suspend(bool suspend);
    bool isSuspended() const;

    // Mode & Quality
    void setMode(NTPMode_t mode);
    void setClockQuality(ClockQuality_t q);
    void setLeapIndicator(uint8_t li);
    void setStratum(uint8_t stratum);
    void setRefID(const char *id);
    void setRootDelay(double seconds);
    void setRootDispersion(double seconds);
    void setPrecision(int8_t precision);
    int8_t measurePrecision();

    // Authentication
    void setAuthEnabled(bool enabled);
    void setAuthCallback(ntp_auth_cb_t cb);
    bool verifyMAC(const uint8_t *packet, size_t packetLen, uint32_t keyId, const uint8_t *digest);

    // Multicast / Broadcast
    void setMulticastEnabled(bool enabled, uint32_t intervalSec = 64);
    void sendBroadcastPacket();

    // Rate Limiting (Token Bucket)
    void setRateLimitEnabled(bool enabled);
    void setRateLimitParams(uint32_t ratePerSec, uint32_t burst);
    void clearRateLimitTable();

    // Leap Second
    void addLeapEvent(uint32_t unixTimestamp, int8_t type);
    void clearLeapEvents();
    uint8_t checkLeapSecond(uint32_t currentUnixTime);

    // Temperature Compensation
    void setTemperatureCompensation(float ppmPerC, float refTempC);
    void setTempCallback(ntp_temp_cb_t cb);

    // Interleaved Mode (v4.0.0: fully automatic, per client — see the
    // ntp_association_t::peerTxTimestamp comment). Just flip it on; no
    // manual timestamp bookkeeping is needed or possible anymore.
    void setInterleavedEnabled(bool enabled);

    // Server-side poll interval clamp (RFC 5905 §7.2)
    void setPollLimits(int8_t minPoll, int8_t maxPoll);

    // Lightweight runtime health/consistency check — safe to call
    // periodically (e.g. from a watchdog task). Does not touch the
    // network; just validates internal invariants and callback wiring.
    ntp_selftest_result_t selfTest() const;

// Response Quality Filter (formerly "Clock Filter") — README §6
#if NTP_ENABLE_CLOCK_FILTER
    void setClockFilterEnabled(bool enabled);
    bool getClockFilterState(int assocIndex, ntp_clock_filter_t &out) const;
    void clearClockFilter(int assocIndex);
    void getFilterStats(int assocIndex, double &offsetSec, double &delaySec, double &dispersion, double &jitterSec,
                        uint8_t &validStages) const;
#endif

    // Diagnostics
    ntp_statistics_t getStatistics() const;
    void resetStatistics();
    void getAssociations(ntp_association_t *out, size_t maxCount, size_t &returnedCount) const;
    void clearAssociations();

    // Config Persistence
    void exportConfig(ntp_config_t &cfg) const;
    void importConfig(const ntp_config_t &cfg);

// Syslog
#if NTP_ENABLE_SYSLOG
    void setSyslogTarget(IPAddress ip, uint16_t port);
    void setSyslogCallback(ntp_syslog_cb_t cb);
#endif

  private:
    AsyncUDP _udp;
    AsyncUDP _udpMulticast;
    bool _running = false;
    bool _suspended = false;

    mutable NTP_LOCK_T _mux = NTP_LOCK_INIT;

    ntp_utc_cb_t _cbUTC = nullptr;
    ntp_subsec_cb_t _cbSubSec = nullptr;
    ntp_leap_cb_t _cbLeap = nullptr;
    ntp_temp_cb_t _cbTemp = nullptr;
    ntp_auth_cb_t _cbAuth = nullptr;

    uint8_t _li = NTP_LI_NO_WARNING;
    uint8_t _version = 4;
    uint8_t _mode = NTP_MODE_SERVER;
    uint8_t _stratum = NTP_STRATUM_UNSYNC;
    ntp_refid_t _refId;
    int8_t _precision = -18;
    ntp_short_t _rootDelay;
    ntp_short_t _rootDispersion;
    ntp_timestamp_t _refTime;
    uint32_t _refTimeUTC = 0;
    ClockQuality_t _clockQuality = ClockQuality::UNKNOWN;

    bool _interleavedEnabled = false;

    bool _authEnabled = false;
    bool _multicastEnabled = false;
    uint32_t _multicastIntervalMs = 64000;
    uint32_t _lastMulticastMs = 0;

    bool _tempCompEnabled = false;
    float _tempPPMperC = 0.0f;
    float _tempRefC = 25.0f;
    // Real correction accumulator for temperature compensation (v4.0.0).
    // Tracks fractional microseconds of drift correction owed since the
    // last time a whole microsecond was folded into a timestamp, so
    // repeated small per-packet corrections don't get lost to rounding.
    uint32_t _tempCompLastMs = 0;
    double _tempCompAccumUs = 0.0;

    bool _rateLimitEnabled = true;
    uint32_t _rateLimitRate = 1;
    uint32_t _rateLimitBurst = 4;
    ntp_token_bucket_t _tokenBuckets[NTP_MAX_ASSOCIATIONS];

    ntp_association_t _associations[NTP_MAX_ASSOCIATIONS];

    int8_t _pollMin = NTP_POLL_MIN;
    int8_t _pollMax = NTP_POLL_MAX;

#if NTP_ENABLE_CLOCK_FILTER
    ntp_clock_filter_t _clockFilters[NTP_MAX_ASSOCIATIONS];
    bool _clockFilterEnabled = true;
#endif

    ntp_leap_event_t _leapEvents[NTP_MAX_LEAP_EVENTS];

    ntp_statistics_t _stats;
    uint32_t _startTimeMs = 0;
    uint32_t _lastDriftCheckMs = 0;
    uint32_t _lastDriftUTC = 0;
    uint32_t _lastDriftUs = 0;

#if NTP_ENABLE_SYSLOG
    IPAddress _syslogIP;
    uint16_t _syslogPort = 514;
    ntp_syslog_cb_t _cbSyslog = nullptr;
#endif

    // Internal methods
    void _processPacket(AsyncUDPPacket &packet);
    void _sendKissOfDeath(AsyncUDPPacket &packet, const char *code);
    void _sendBroadcastPacketInternal();
    bool _checkTokenBucket(uint32_t clientIP);
    int _findOrCreateAssociation(uint32_t ip);
    void _updateAssociation(int idx, const ntp_packet_t &req, int32_t offsetUs, int32_t delayUs);
    void _updateDrift();
    void _updateRefTime();
    void _applyTemperatureCompensation(uint32_t &utcSec, uint32_t &us);
    uint8_t _effectiveLI(uint32_t utcSec);
    void _logSyslog(const char *fmt, ...);
    static bool _constantTimeEquals(const uint8_t *a, const uint8_t *b, size_t len);

    // Extension-field / legacy-MAC location (v4.0.0). Walks RFC 7822 TLV
    // fields (if any) and returns a pointer to the trailing legacy MAC
    // (4-byte KeyID + digest) once found. Returns false if no MAC of the
    // configured length is present, or if a malformed field length is
    // encountered (fails safe rather than reading out of bounds).
    static bool _locateLegacyMAC(const uint8_t *ext, size_t extLen, size_t digestLen, const uint8_t **outMac,
                                  size_t *outMacLen);

    // Folds a signed microsecond delta into (utcSec, us), carrying between
    // the two fields correctly in either direction. Extracted as its own
    // static helper (v4.0.0) so it can be unit-tested independently of
    // millis()/callback timing — see tests/test_ntp.cpp.
    static void _foldMicroseconds(int64_t deltaUs, uint32_t &utcSec, uint32_t &us);

#if NTP_ENABLE_CLOCK_FILTER
    void _clockFilter(int assocIdx, double offset, double delay, double disp);
    void _initClockFilter(int assocIdx);
    double _rootDistance(int assocIdx) const;
    bool _fitAssociation(int assocIdx) const;
#endif

    // Helpers
    static ntp_timestamp_t _makeTimestamp(uint32_t utcSec, uint32_t us);
    static ntp_short_t _secondsToShort(double sec);
    static uint32_t _usToFraction(uint32_t us);
    static double _shortToSeconds(ntp_short_t s);
    static int32_t _estimateOffset(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2, const ntp_timestamp_t &t3,
                                   const ntp_timestamp_t &t4);
    static int32_t _estimateDelay(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2, const ntp_timestamp_t &t3,
                                  const ntp_timestamp_t &t4);

#ifdef NTP_TESTING
    // White-box test access only. NTP_TESTING is never defined in normal
    // builds — the native unit test suite in tests/ defines it to reach
    // private helpers directly instead of duplicating logic or relying on
    // fragile #define private public tricks.
    friend class NTPServerTestAccess;
#endif
};

#endif //_NTP_SERVER_ADVANCED_H_
