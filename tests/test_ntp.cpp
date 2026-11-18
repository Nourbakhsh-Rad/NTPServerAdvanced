//-----------------------------------------------------------------------------
// Native unit tests for AdvancedNTPServer internals.
//
// This file is built on a desktop only. It needs NTP_TESTING defined (which
// the Arduino/PlatformIO builds never set) to reach private members through
// the test-only friend class below, so it must not be copied into a sketch
// folder. It lives in tests/, which library builds ignore. Run it via
//    `cd tests && make run`. See README §14.
//
// These run on the host (no ESP32/ESP8266 hardware needed) against the
// Arduino/AsyncUDP stub headers in tests/stubs/. They exercise the parts of
// the library that are pure logic — fixed-point math, the token bucket, the
// association table, the extension-field walker, constant-time comparison,
// and the microsecond-folding helper — via a small friend-class test
// harness (NTP_TESTING) rather than #define private public tricks.
//
// Build & run:
//   cd tests && make run
//
// Exit code is 0 iff every test passed.
//-----------------------------------------------------------------------------
#include "NTPServer_Advanced.h"
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                              \
        if (cond) {                                                                                                    \
            g_pass++;                                                                                                   \
        } else {                                                                                                        \
            g_fail++;                                                                                                    \
            printf("  FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                                   \
        }                                                                                                              \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)                                                                                                     \
    do {                                                                                                               \
        printf("-- %s --\n", #name);                                                                                    \
        name();                                                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// White-box access (friend of AdvancedNTPServer, enabled by NTP_TESTING)
// ---------------------------------------------------------------------------
class NTPServerTestAccess {
  public:
    static uint32_t usToFraction(uint32_t us) { return AdvancedNTPServer::_usToFraction(us); }
    static ntp_short_t secondsToShort(double s) { return AdvancedNTPServer::_secondsToShort(s); }
    static double shortToSeconds(ntp_short_t s) { return AdvancedNTPServer::_shortToSeconds(s); }
    static ntp_timestamp_t makeTimestamp(uint32_t sec, uint32_t us) { return AdvancedNTPServer::_makeTimestamp(sec, us); }

    static int32_t estimateOffset(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2, const ntp_timestamp_t &t3,
                                   const ntp_timestamp_t &t4) {
        return AdvancedNTPServer::_estimateOffset(t1, t2, t3, t4);
    }
    static int32_t estimateDelay(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2, const ntp_timestamp_t &t3,
                                  const ntp_timestamp_t &t4) {
        return AdvancedNTPServer::_estimateDelay(t1, t2, t3, t4);
    }

    static bool constantTimeEquals(const uint8_t *a, const uint8_t *b, size_t len) {
        return AdvancedNTPServer::_constantTimeEquals(a, b, len);
    }

    static bool locateLegacyMAC(const uint8_t *ext, size_t extLen, size_t digestLen, const uint8_t **outMac,
                                 size_t *outMacLen) {
        return AdvancedNTPServer::_locateLegacyMAC(ext, extLen, digestLen, outMac, outMacLen);
    }

    static void foldMicroseconds(int64_t deltaUs, uint32_t &utcSec, uint32_t &us) {
        AdvancedNTPServer::_foldMicroseconds(deltaUs, utcSec, us);
    }

    static bool checkTokenBucket(AdvancedNTPServer &s, uint32_t ip) { return s._checkTokenBucket(ip); }
    static int findOrCreateAssociation(AdvancedNTPServer &s, uint32_t ip) { return s._findOrCreateAssociation(ip); }
    static void setRateLimit(AdvancedNTPServer &s, uint32_t rate, uint32_t burst) { s.setRateLimitParams(rate, burst); }
    static int8_t pollMin(const AdvancedNTPServer &s) { return s._pollMin; }
    static int8_t pollMax(const AdvancedNTPServer &s) { return s._pollMax; }
    static void process(AdvancedNTPServer &s, AsyncUDPPacket &p) { s._processPacket(p); }
    static void updateDrift(AdvancedNTPServer &s) { s._updateDrift(); }
};

// ---------------------------------------------------------------------------
// Fixed-point math
// ---------------------------------------------------------------------------
TEST(test_usToFraction_roundtrip) {
    CHECK(NTPServerTestAccess::usToFraction(0) == 0);
    // 500000us == half a second == 0x80000000 in NTP fraction (± rounding)
    uint32_t half = NTPServerTestAccess::usToFraction(500000);
    CHECK(half > 0x7FFF0000u && half < 0x80010000u);
    // Must not exceed the 999999 clamp
    uint32_t maxed = NTPServerTestAccess::usToFraction(5000000); // out of range, should clamp to 999999
    uint32_t atLimit = NTPServerTestAccess::usToFraction(999999);
    CHECK(maxed == atLimit);
}

TEST(test_secondsToShort_clamps) {
    ntp_short_t neg = NTPServerTestAccess::secondsToShort(-5.0);
    CHECK(neg.raw == 0);
    ntp_short_t huge = NTPServerTestAccess::secondsToShort(999999.0);
    ntp_short_t capped = NTPServerTestAccess::secondsToShort(65535.9999);
    CHECK(huge.raw == capped.raw);
    // Round-trip a mid-range value approximately
    double back = NTPServerTestAccess::shortToSeconds(NTPServerTestAccess::secondsToShort(1.5));
    CHECK(back > 1.49 && back < 1.51);
}

// This is the specific bug fixed in v3.5.0: fraction subtraction used to
// wrap in uint32_t *before* the cast to int64_t, turning small negative
// differences into huge positive numbers. Verify the fix directly.
TEST(test_estimateOffset_negative_fraction_diff) {
    ntp_timestamp_t t1 = NTPServerTestAccess::makeTimestamp(1000, 900000); // large fraction
    ntp_timestamp_t t2 = NTPServerTestAccess::makeTimestamp(1000, 100000); // smaller fraction, same second
    // t2.fraction - t1.fraction is negative: must not appear as a huge
    // positive offset after the fix.
    int32_t offset = NTPServerTestAccess::estimateOffset(t1, t2, t2, t2);
    CHECK(offset < 100000);  // sane range in microseconds
    CHECK(offset > -900000); // and not wrapped to a huge positive number
}

TEST(test_estimateDelay_symmetric_roundtrip) {
    // t1=t4=0s, t2=t3=0.001s -> classic zero-delay, symmetric round trip
    ntp_timestamp_t t1 = NTPServerTestAccess::makeTimestamp(1000, 0);
    ntp_timestamp_t t2 = NTPServerTestAccess::makeTimestamp(1000, 1000);
    ntp_timestamp_t t3 = NTPServerTestAccess::makeTimestamp(1000, 1000);
    ntp_timestamp_t t4 = NTPServerTestAccess::makeTimestamp(1000, 0);
    int32_t delay = NTPServerTestAccess::estimateDelay(t1, t2, t3, t4);
    // (t4-t1) - (t3-t2) = (0-0) - (0) = 0
    CHECK(delay > -50 && delay < 50); // near zero, allow for fraction rounding
}

// ---------------------------------------------------------------------------
// Constant-time comparison
// ---------------------------------------------------------------------------
TEST(test_constantTimeEquals) {
    uint8_t a[16], b[16];
    memset(a, 0xAB, sizeof(a));
    memset(b, 0xAB, sizeof(b));
    CHECK(NTPServerTestAccess::constantTimeEquals(a, b, 16) == true);
    b[15] ^= 0x01;
    CHECK(NTPServerTestAccess::constantTimeEquals(a, b, 16) == false);
    b[15] ^= 0x01; // restore
    b[0] ^= 0x01;  // difference at the *start* must still be caught
    CHECK(NTPServerTestAccess::constantTimeEquals(a, b, 16) == false);
}

// ---------------------------------------------------------------------------
// Extension-field / legacy-MAC walker
// ---------------------------------------------------------------------------
TEST(test_locateLegacyMAC_bare) {
    uint8_t buf[20] = {0}; // 4-byte KeyID + 16-byte digest, nothing else
    const uint8_t *mac = nullptr;
    size_t macLen = 0;
    bool found = NTPServerTestAccess::locateLegacyMAC(buf, sizeof(buf), 16, &mac, &macLen);
    CHECK(found == true);
    CHECK(mac == buf);
    CHECK(macLen == 20);
}

TEST(test_locateLegacyMAC_skips_leading_tlv) {
    // One 8-byte TLV field [type:2][length:2=8][value:4], then a 20-byte
    // legacy MAC. The walker must skip the TLV and find the MAC after it.
    uint8_t buf[8 + 20];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00;
    buf[1] = 0x02; // arbitrary type
    buf[2] = 0x00;
    buf[3] = 0x08; // length = 8 (covers this whole field)
    // buf[8..27] would be the legacy MAC (left as zero bytes)
    const uint8_t *mac = nullptr;
    size_t macLen = 0;
    bool found = NTPServerTestAccess::locateLegacyMAC(buf, sizeof(buf), 16, &mac, &macLen);
    CHECK(found == true);
    CHECK(mac == buf + 8);
    CHECK(macLen == 20);
}

TEST(test_locateLegacyMAC_rejects_malformed_length) {
    // A field claiming a length larger than what remains must not be
    // walked into (would read out of bounds) — expect a clean "not found".
    uint8_t buf[10];
    memset(buf, 0, sizeof(buf));
    buf[2] = 0x00;
    buf[3] = 0xFF; // length = 255, way past the 10-byte buffer
    const uint8_t *mac = nullptr;
    size_t macLen = 0;
    bool found = NTPServerTestAccess::locateLegacyMAC(buf, sizeof(buf), 16, &mac, &macLen);
    CHECK(found == false);
}

TEST(test_locateLegacyMAC_no_mac_present) {
    uint8_t buf[8] = {0}; // too short to ever contain a 20-byte MD5 MAC
    const uint8_t *mac = nullptr;
    size_t macLen = 0;
    bool found = NTPServerTestAccess::locateLegacyMAC(buf, sizeof(buf), 16, &mac, &macLen);
    CHECK(found == false);
}

// ---------------------------------------------------------------------------
// Microsecond folding (temperature compensation carry logic)
// ---------------------------------------------------------------------------
TEST(test_foldMicroseconds_positive_carry) {
    uint32_t sec = 100, us = 999990;
    NTPServerTestAccess::foldMicroseconds(20, sec, us); // +20us should roll into the next second
    CHECK(sec == 101);
    CHECK(us == 10);
}

TEST(test_foldMicroseconds_negative_carry) {
    uint32_t sec = 100, us = 5;
    NTPServerTestAccess::foldMicroseconds(-20, sec, us); // -20us should borrow from the previous second
    CHECK(sec == 99);
    CHECK(us == 999985);
}

TEST(test_foldMicroseconds_no_carry) {
    uint32_t sec = 100, us = 500000;
    NTPServerTestAccess::foldMicroseconds(100, sec, us);
    CHECK(sec == 100);
    CHECK(us == 500100);
}

TEST(test_foldMicroseconds_never_goes_before_epoch) {
    uint32_t sec = 0, us = 5;
    NTPServerTestAccess::foldMicroseconds(-1000000, sec, us); // would go to sec=-1
    CHECK(sec == 0); // clamped, not wrapped to a huge uint32_t
}

// ---------------------------------------------------------------------------
// Token bucket rate limiting
// ---------------------------------------------------------------------------
TEST(test_token_bucket_burst_then_deny) {
    AdvancedNTPServer server;
    NTPServerTestAccess::setRateLimit(server, /*rate=*/1, /*burst=*/3);

    uint32_t ip = 0x0A000001; // 10.0.0.1
    // millis() in the test stub is constant, so no time-based refill
    // happens between calls — this deterministically exercises the
    // admission/denial logic in isolation. A fresh bucket is initialized
    // with tokens = burst - 1 (the creating call itself is the first
    // admission), so burst=3 admits exactly 3 packets before denying.
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ip) == true);  // admission 1 (bucket created)
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ip) == true);  // admission 2
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ip) == true);  // admission 3 (burst exhausted)
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ip) == false); // denied
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ip) == false); // still denied
}

TEST(test_token_bucket_per_ip_isolation) {
    AdvancedNTPServer server;
    NTPServerTestAccess::setRateLimit(server, 1, 1);

    uint32_t ipA = 0x0A000001;
    uint32_t ipB = 0x0A000002;
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ipA) == true);
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ipA) == false); // A exhausted
    CHECK(NTPServerTestAccess::checkTokenBucket(server, ipB) == true);  // B independent of A
}

// ---------------------------------------------------------------------------
// Association table
// ---------------------------------------------------------------------------
TEST(test_association_same_ip_returns_same_slot) {
    AdvancedNTPServer server;
    uint32_t ip = 0x0A000001;
    int idx1 = NTPServerTestAccess::findOrCreateAssociation(server, ip);
    int idx2 = NTPServerTestAccess::findOrCreateAssociation(server, ip);
    CHECK(idx1 >= 0);
    CHECK(idx1 == idx2);
}

TEST(test_association_different_ips_get_different_slots) {
    AdvancedNTPServer server;
    int idx1 = NTPServerTestAccess::findOrCreateAssociation(server, 0x0A000001);
    int idx2 = NTPServerTestAccess::findOrCreateAssociation(server, 0x0A000002);
    CHECK(idx1 != idx2);
}

// ---------------------------------------------------------------------------
// Poll interval limits
// ---------------------------------------------------------------------------
TEST(test_poll_limits_applied) {
    AdvancedNTPServer server;
    server.setPollLimits(5, 9);
    CHECK(NTPServerTestAccess::pollMin(server) == 5);
    CHECK(NTPServerTestAccess::pollMax(server) == 9);
}

TEST(test_poll_limits_rejects_inverted_range) {
    AdvancedNTPServer server;
    server.setPollLimits(4, 10); // establish a known-good baseline
    server.setPollLimits(9, 5);  // min > max: must be rejected (no-op)
    CHECK(NTPServerTestAccess::pollMin(server) == 4);
    CHECK(NTPServerTestAccess::pollMax(server) == 10);
}

// ---------------------------------------------------------------------------
// selfTest()
// ---------------------------------------------------------------------------
TEST(test_selftest_fails_without_callbacks) {
    AdvancedNTPServer server;
    ntp_selftest_result_t r = server.selfTest();
    CHECK(r.ok == false);
    CHECK(r.callbacksValid == false);
}

TEST(test_selftest_passes_after_begin) {
    AdvancedNTPServer server;
    bool started = server.begin(
        123, []() -> uint32_t { return 1000; }, []() -> uint32_t { return 0; });
    CHECK(started == true);
    ntp_selftest_result_t r = server.selfTest();
    CHECK(r.callbacksValid == true);
    CHECK(r.associationTableConsistent == true);
    CHECK(r.tokenBucketTableConsistent == true);
}


// ---------------------------------------------------------------------------
// Whole-packet tests: build a raw client request, push it through the real
// _processPacket(), and inspect the bytes the server wrote back.
// ---------------------------------------------------------------------------
static uint32_t g_utc = 1700000000;
static uint32_t g_us = 250000;
static uint8_t g_cbLeapValue = 0;

static uint32_t cbUtc() { return g_utc; }
static uint32_t cbUs() { return g_us; }

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// A recognisable, asymmetric client request (v4, mode 3). Non-zero values
// in the multi-byte fields matter: they make a byte-order mix-up visible.
static std::vector<uint8_t> makeClientPacket() {
    std::vector<uint8_t> b(48, 0);
    b[0] = (0 << 6) | (4 << 3) | 3;
    b[1] = 0;
    b[2] = 6;
    b[3] = 0xEC;
    put32(&b[4], 0x00010203);  // rootDelay
    put32(&b[8], 0x04050607);  // rootDispersion
    put32(&b[12], 0x41424344); // refId
    put32(&b[40], 0xE8000001); // txTime seconds (T1)
    put32(&b[44], 0x12345678); // txTime fraction
    return b;
}

static AdvancedNTPServer *makeServer() {
    AdvancedNTPServer *s = new AdvancedNTPServer();
    s->setRateLimitEnabled(false);
    s->begin(123, cbUtc, cbUs, nullptr);
    s->setMode(NTPMode_t::SYNC);
    return s;
}

TEST(test_response_echoes_client_transmit_time) {
    AdvancedNTPServer *s = makeServer();
    std::vector<uint8_t> rq = makeClientPacket();
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000001);
    NTPServerTestAccess::process(*s, pkt);

    const std::vector<uint8_t> &r = pkt.sent();
    CHECK(r.size() == 48);
    CHECK((r[0] & 7) == 4);        // mode: server
    CHECK(((r[0] >> 3) & 7) == 4); // version mirrored
    CHECK(r[1] == 2);              // stratum from SYNC mode
    CHECK(memcmp(&r[24], &rq[40], 8) == 0); // origTime == client's T1, byte for byte
    CHECK(get32(&r[32]) == g_utc + 2208988800u); // rxTime seconds
    delete s;
}

TEST(test_unauthenticated_reply_is_plain_48_bytes_when_auth_disabled) {
    AdvancedNTPServer *s = makeServer();
    std::vector<uint8_t> rq = makeClientPacket();
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000002);
    NTPServerTestAccess::process(*s, pkt);
    CHECK(pkt.sent().size() == 48);
    delete s;
}

TEST(test_rate_limit_kod_echoes_origin_timestamp) {
    AdvancedNTPServer *s = new AdvancedNTPServer();
    s->begin(123, cbUtc, cbUs, nullptr);
    s->setMode(NTPMode_t::SYNC);
    s->setRateLimitEnabled(true);
    s->setRateLimitParams(1, 1); // burst of one
    std::vector<uint8_t> rq = makeClientPacket();

    AsyncUDPPacket first(rq.data(), rq.size(), 0x0A000003);
    NTPServerTestAccess::process(*s, first);
    CHECK(first.sent().size() == 48);
    CHECK(first.sent()[1] == 2); // normal reply

    AsyncUDPPacket second(rq.data(), rq.size(), 0x0A000003);
    NTPServerTestAccess::process(*s, second);
    const std::vector<uint8_t> &k = second.sent();
    CHECK(k.size() == 48);
    CHECK(k[1] == 0);                    // stratum 0 => Kiss-o'-Death
    CHECK(memcmp(&k[12], "RATE", 4) == 0);
    CHECK(memcmp(&k[24], &rq[40], 8) == 0); // clients drop a KoD without this
    CHECK(s->getStatistics().kissOfDeathSent == 1);
    CHECK(s->getStatistics().packetsDropped == 1);
    delete s;
}

// ---- Authentication -------------------------------------------------------
// Toy "digest" (not cryptographic) so the tests can verify *which bytes* the
// library hands to the callback without needing a real MD5 on the host.
static std::vector<uint8_t> g_lastSignedInput;
static bool testAuthCb(uint32_t keyId, const uint8_t *data, size_t len, uint8_t *out) {
    if (keyId != 7)
        return false;
    g_lastSignedInput.assign(data, data + len);
    for (int i = 0; i < NTP_AUTH_DIGEST_LEN; i++)
        out[i] = (uint8_t)(0x5A + i);
    for (size_t i = 0; i < len; i++)
        out[i % NTP_AUTH_DIGEST_LEN] = (uint8_t)(out[i % NTP_AUTH_DIGEST_LEN] * 31 + data[i]);
    return true;
}

static std::vector<uint8_t> makeAuthedPacket(bool corrupt) {
    std::vector<uint8_t> rq = makeClientPacket();
    uint8_t digest[NTP_AUTH_DIGEST_LEN];
    testAuthCb(7, rq.data(), rq.size(), digest);
    if (corrupt)
        digest[3] ^= 0x01;
    uint8_t kid[4];
    put32(kid, 7);
    rq.insert(rq.end(), kid, kid + 4);
    rq.insert(rq.end(), digest, digest + NTP_AUTH_DIGEST_LEN);
    return rq;
}

TEST(test_auth_digest_checked_over_wire_bytes_and_reply_is_signed) {
    AdvancedNTPServer *s = makeServer();
    s->setAuthEnabled(true);
    s->setAuthCallback(testAuthCb);

    std::vector<uint8_t> rq = makeAuthedPacket(false);
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000004);
    NTPServerTestAccess::process(*s, pkt);

    const std::vector<uint8_t> &r = pkt.sent();
    CHECK(s->getStatistics().packetsAuthFail == 0);
    CHECK(r.size() == 48 + 4 + NTP_AUTH_DIGEST_LEN); // reply carries keyId + digest
    CHECK(r[1] == 2);                                // a real reply, not a KoD
    if (r.size() == 48 + 4 + NTP_AUTH_DIGEST_LEN) {
        CHECK(get32(&r[48]) == 7);
        uint8_t expect[NTP_AUTH_DIGEST_LEN];
        testAuthCb(7, r.data(), 48, expect);
        CHECK(memcmp(&r[52], expect, NTP_AUTH_DIGEST_LEN) == 0);
    }
    delete s;
}

TEST(test_auth_bad_digest_gets_auth_kod) {
    AdvancedNTPServer *s = makeServer();
    s->setAuthEnabled(true);
    s->setAuthCallback(testAuthCb);

    std::vector<uint8_t> rq = makeAuthedPacket(true);
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000005);
    NTPServerTestAccess::process(*s, pkt);

    const std::vector<uint8_t> &r = pkt.sent();
    CHECK(r.size() == 48);
    CHECK(r[1] == 0);
    CHECK(memcmp(&r[12], "AUTH", 4) == 0);
    CHECK(memcmp(&r[24], &rq[40], 8) == 0);
    CHECK(s->getStatistics().packetsAuthFail == 1);
    delete s;
}

TEST(test_auth_missing_mac_gets_auth_kod) {
    AdvancedNTPServer *s = makeServer();
    s->setAuthEnabled(true);
    s->setAuthCallback(testAuthCb);

    std::vector<uint8_t> rq = makeClientPacket(); // no MAC appended
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000006);
    NTPServerTestAccess::process(*s, pkt);
    CHECK(pkt.sent().size() == 48);
    CHECK(memcmp(&pkt.sent()[12], "AUTH", 4) == 0);
    delete s;
}

// ---- Leap indicator ---------------------------------------------------------
static uint8_t leapCb() { return g_cbLeapValue; }

static uint8_t liOf(AdvancedNTPServer &s, uint32_t ip) {
    std::vector<uint8_t> rq = makeClientPacket();
    AsyncUDPPacket pkt(rq.data(), rq.size(), ip);
    NTPServerTestAccess::process(s, pkt);
    return pkt.sent().empty() ? 0xFF : (pkt.sent()[0] >> 6);
}

TEST(test_leap_event_table_sets_li_inside_24h_window) {
    AdvancedNTPServer *s = makeServer();
    CHECK(liOf(*s, 0x0A000010) == 0);
    s->addLeapEvent(g_utc + 3600, +1); // one hour away
    CHECK(liOf(*s, 0x0A000011) == NTP_LI_ADD_SECOND);
    s->clearLeapEvents();
    s->addLeapEvent(g_utc + 3600, -1);
    CHECK(liOf(*s, 0x0A000012) == NTP_LI_DEL_SECOND);
    s->clearLeapEvents();
    s->addLeapEvent(g_utc + 3 * 86400, +1); // too far away: no warning yet
    CHECK(liOf(*s, 0x0A000013) == NTP_LI_NO_WARNING);
    delete s;
}

TEST(test_leap_callback_overrides_table) {
    AdvancedNTPServer *s = new AdvancedNTPServer();
    s->setRateLimitEnabled(false);
    s->begin(123, cbUtc, cbUs, leapCb);
    s->setMode(NTPMode_t::SYNC);
    g_cbLeapValue = 1;
    CHECK(liOf(*s, 0x0A000020) == NTP_LI_ADD_SECOND);
    g_cbLeapValue = 2;
    CHECK(liOf(*s, 0x0A000021) == NTP_LI_DEL_SECOND);
    g_cbLeapValue = 0;
    CHECK(liOf(*s, 0x0A000022) == NTP_LI_NO_WARNING);
    delete s;
}

TEST(test_not_in_sync_li_is_never_overridden) {
    AdvancedNTPServer *s = makeServer();
    s->setMode(NTPMode_t::ALARM); // LI = 3
    s->addLeapEvent(g_utc + 3600, +1);
    CHECK(liOf(*s, 0x0A000030) == NTP_LI_NOT_IN_SYNC);
    delete s;
}

// ---- Drift estimate ---------------------------------------------------------
TEST(test_drift_estimate_ppm) {
    fake_millis() = 0;
    g_utc = 1000;
    g_us = 0;
    AdvancedNTPServer *s = makeServer();
    // 60 s of wall time, but the UTC source advanced 60.0006 s => +10 ppm.
    fake_millis() = 60000;
    g_utc = 1060;
    g_us = 600;
    NTPServerTestAccess::updateDrift(*s);
    float ppm = s->getStatistics().estimatedDriftPPM;
    CHECK(ppm > 9.9f && ppm < 10.1f);
    delete s;
    fake_millis() = 0;
    g_utc = 1700000000;
    g_us = 250000;
}

// ---- Year 2036 / NTP era rollover ---------------------------------------------
// NTP seconds wrap to 0 at Unix time 2^32 - 2208988800 = 2085978496
// (2036-02-07 06:28:16 UTC). Timestamps on the wire are *supposed* to wrap;
// what must not break is arithmetic that compares across the boundary.
static const uint32_t ERA_BOUNDARY_UNIX = 2085978496u;

TEST(test_era_wire_timestamp_wraps_correctly) {
    g_utc = ERA_BOUNDARY_UNIX + 5;
    AdvancedNTPServer *s = makeServer();
    std::vector<uint8_t> rq = makeClientPacket();
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000040);
    NTPServerTestAccess::process(*s, pkt);
    CHECK(pkt.sent().size() == 48);
    CHECK(get32(&pkt.sent()[32]) == 5); // rxTime seconds: wrapped, as RFC 5905 intends
    delete s;
    g_utc = 1700000000;
}

TEST(test_era_offset_and_delay_across_rollover) {
    // T1 just before the rollover, T2/T3/T4 just after: a plain integer
    // subtraction would be off by ~4.29e9 seconds.
    ntp_timestamp_t t1 = {0xFFFFFFFFu, 0x80000000u}; // ...xFF.5
    ntp_timestamp_t t2 = {0x00000000u, 0x00000000u}; // 0.0  (0.5 s later)
    ntp_timestamp_t t3 = {0x00000000u, 0x00000000u};
    ntp_timestamp_t t4 = {0x00000000u, 0x80000000u}; // 0.5
    int32_t delay = NTPServerTestAccess::estimateDelay(t1, t2, t3, t4);
    CHECK(delay > 990000 && delay < 1010000); // (t4-t1) - (t3-t2) = 1.0 s
    int32_t off = NTPServerTestAccess::estimateOffset(t1, t2, t3, t4);
    CHECK(off > -1000 && off < 1000 + 1000000); // small, not billions of microseconds
}

TEST(test_era_leap_window_before_and_after_rollover) {
    AdvancedNTPServer *s = makeServer();
    s->addLeapEvent(ERA_BOUNDARY_UNIX + 3600, +1);
    g_utc = ERA_BOUNDARY_UNIX - 100; // event is 1h 1m 40s away, NTP seconds not yet wrapped
    CHECK(liOf(*s, 0x0A000050) == NTP_LI_ADD_SECOND);
    g_utc = ERA_BOUNDARY_UNIX + 10; // NTP seconds have wrapped, event still 59 min away
    CHECK(liOf(*s, 0x0A000051) == NTP_LI_ADD_SECOND);
    g_utc = ERA_BOUNDARY_UNIX + 7200; // event has passed
    CHECK(liOf(*s, 0x0A000052) == NTP_LI_NO_WARNING);
    delete s;
    g_utc = 1700000000;
}

TEST(test_control_message_is_counted_and_dropped) {
    AdvancedNTPServer *s = makeServer();
    std::vector<uint8_t> rq = makeClientPacket();
    rq[0] = (0 << 6) | (4 << 3) | 6; // mode 6 (ntpq)
    AsyncUDPPacket pkt(rq.data(), rq.size(), 0x0A000060);
    NTPServerTestAccess::process(*s, pkt);
    CHECK(pkt.writes() == 0); // no reply at all
    CHECK(s->getStatistics().controlMsgsReceived == 1);
    CHECK(s->getStatistics().packetsSent == 0);
    delete s;
}

// ---- importConfig sanitising -------------------------------------------------
TEST(test_import_config_rejects_garbage) {
    AdvancedNTPServer s;
    ntp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 99;
    cfg.stratum = 200;
    cfg.li = 9;
    cfg.precision = 100;
    s.importConfig(cfg);
    ntp_config_t out;
    s.exportConfig(out);
    CHECK(out.version == 4);
    CHECK(out.stratum == NTP_STRATUM_UNSYNC);
    CHECK(out.li == NTP_LI_NOT_IN_SYNC);
    CHECK(out.precision == -6);
    CHECK(out.rateLimitRate >= 1 && out.rateLimitBurst >= 1);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    RUN(test_usToFraction_roundtrip);
    RUN(test_secondsToShort_clamps);
    RUN(test_estimateOffset_negative_fraction_diff);
    RUN(test_estimateDelay_symmetric_roundtrip);
    RUN(test_constantTimeEquals);
    RUN(test_locateLegacyMAC_bare);
    RUN(test_locateLegacyMAC_skips_leading_tlv);
    RUN(test_locateLegacyMAC_rejects_malformed_length);
    RUN(test_locateLegacyMAC_no_mac_present);
    RUN(test_foldMicroseconds_positive_carry);
    RUN(test_foldMicroseconds_negative_carry);
    RUN(test_foldMicroseconds_no_carry);
    RUN(test_foldMicroseconds_never_goes_before_epoch);
    RUN(test_token_bucket_burst_then_deny);
    RUN(test_token_bucket_per_ip_isolation);
    RUN(test_association_same_ip_returns_same_slot);
    RUN(test_association_different_ips_get_different_slots);
    RUN(test_poll_limits_applied);
    RUN(test_poll_limits_rejects_inverted_range);
    RUN(test_selftest_fails_without_callbacks);
    RUN(test_selftest_passes_after_begin);
    RUN(test_response_echoes_client_transmit_time);
    RUN(test_unauthenticated_reply_is_plain_48_bytes_when_auth_disabled);
    RUN(test_rate_limit_kod_echoes_origin_timestamp);
    RUN(test_auth_digest_checked_over_wire_bytes_and_reply_is_signed);
    RUN(test_auth_bad_digest_gets_auth_kod);
    RUN(test_auth_missing_mac_gets_auth_kod);
    RUN(test_leap_event_table_sets_li_inside_24h_window);
    RUN(test_leap_callback_overrides_table);
    RUN(test_not_in_sync_li_is_never_overridden);
    RUN(test_drift_estimate_ppm);
    RUN(test_import_config_rejects_garbage);
    RUN(test_control_message_is_counted_and_dropped);
    RUN(test_era_wire_timestamp_wraps_correctly);
    RUN(test_era_offset_and_delay_across_rollover);
    RUN(test_era_leap_window_before_and_after_rollover);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
