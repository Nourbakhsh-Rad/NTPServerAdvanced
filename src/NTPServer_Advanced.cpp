//-----------------------------------------------------------------------------
// Industrial NTP Server v4.1.0 — Advanced Edition + Response Quality Filter
//-----------------------------------------------------------------------------
#include "NTPServer_Advanced.h"

#include <stdarg.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
AdvancedNTPServer::AdvancedNTPServer() {
    memset(&_stats, 0, sizeof(_stats));
    memset(&_associations, 0, sizeof(_associations));
    memset(&_tokenBuckets, 0, sizeof(_tokenBuckets));
    memset(&_leapEvents, 0, sizeof(_leapEvents));
    memset(&_refId, 0, sizeof(_refId));
    memset(&_rootDelay, 0, sizeof(_rootDelay));
    memset(&_rootDispersion, 0, sizeof(_rootDispersion));
    memset(&_refTime, 0, sizeof(_refTime));
    memcpy(_refId.str, "INIT", 4);

#if NTP_ENABLE_CLOCK_FILTER
    memset(_clockFilters, 0, sizeof(_clockFilters));

    for (int i = 0; i < NTP_MAX_ASSOCIATIONS; i++) {
        _initClockFilter(i);
    }
#endif
}

AdvancedNTPServer::~AdvancedNTPServer() { end(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool AdvancedNTPServer::begin(uint16_t port, ntp_utc_cb_t getUTC, ntp_subsec_cb_t getSubSec, ntp_leap_cb_t getLeap, ntp_temp_cb_t getTemp) {
    if (_running) {
        NTP_LOGW("Already running");
        return false;
    }

    if (!getUTC || !getSubSec) {
        NTP_LOGE("UTC and SubSec callbacks required");
        return false;
    }

    _cbUTC = getUTC;
    _cbSubSec = getSubSec;
    _cbLeap = getLeap;
    _cbTemp = getTemp;

    if (_precision == -18)
        measurePrecision();

    _updateRefTime();
    _startTimeMs = millis();
    _lastDriftCheckMs = _startTimeMs;
    _lastDriftUTC = _cbUTC();
    _lastDriftUs = _cbSubSec();

    if (_udp.listen(port)) {
        _running = true;
        _udp.onPacket([this](AsyncUDPPacket pkt) { this->_processPacket(pkt); });

        if (_multicastEnabled) {
            _udpMulticast.listenMulticast(IPAddress(224, 0, 1, 1), port);
        }

        NTP_LOGI("NTP Server v%s started on port %u, precision=%d, RQF=%d", NTP_SERVER_VERSION_STRING, port,
                 _precision, (int)NTP_ENABLE_CLOCK_FILTER);
        _logSyslog("NTP server started, precision=%d", _precision);

        return true;
    }

    NTP_LOGE("Failed to bind UDP port %u", port);
    return false;
}

void AdvancedNTPServer::end() {
    if (_running) {
        _udp.close();
        _udpMulticast.close();

        _running = false;
        NTP_LOGI("Server stopped");
    }
}

bool AdvancedNTPServer::isRunning() const { return _running; }

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
void AdvancedNTPServer::suspend(bool suspend) {
    _suspended = suspend;
    NTP_LOGI("Server %s", suspend ? "suspended" : "resumed");
}

bool AdvancedNTPServer::isSuspended() const { return _suspended; }

// ---------------------------------------------------------------------------
// Mode & Quality
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setMode(NTPMode_t mode) {
    switch (mode) {
    case NTPMode_t::GPS:
        _li = NTP_LI_NO_WARNING;
        _version = 4;
        _mode = NTP_MODE_SERVER;
        _stratum = NTP_STRATUM_PRIMARY;
        memcpy(_refId.str, "GPS\0", 4);
        setRootDelay(0.0);
        setRootDispersion(0.001);
        _clockQuality = ClockQuality::EXCELLENT;
        break;

    case NTPMode_t::SYNC:
        _li = NTP_LI_NO_WARNING;
        _version = 4;
        _mode = NTP_MODE_SERVER;
        _stratum = 2;
        memcpy(_refId.str, "LCLS", 4);
        setRootDelay(0.010);
        setRootDispersion(0.050);
        _clockQuality = ClockQuality::GOOD;
        break;

    case NTPMode_t::VALID:
        _li = NTP_LI_NO_WARNING;
        _version = 4;
        _mode = NTP_MODE_SERVER;
        _stratum = 5;
        memcpy(_refId.str, "LCLV", 4);
        setRootDelay(0.100);
        setRootDispersion(0.500);
        _clockQuality = ClockQuality::WARMUP;
        break;

    default:
        _li = NTP_LI_NOT_IN_SYNC;
        _version = 4;
        _mode = NTP_MODE_SERVER;
        _stratum = NTP_STRATUM_UNSPECIFIED;
        memcpy(_refId.str, "INIT", 4);
        setRootDelay(1.0);
        setRootDispersion(NTP_CF_MAXDISP);
        _clockQuality = ClockQuality::UNKNOWN;
        break;
    }

    _updateRefTime();
    NTP_LOGI("Mode set to %d, stratum=%u, li=%u", (int)mode, _stratum, _li);
}

void AdvancedNTPServer::setClockQuality(ClockQuality_t q) { _clockQuality = q; }
void AdvancedNTPServer::setLeapIndicator(uint8_t li) {
    if (li <= 3) {
        _li = li;
        _updateRefTime();
    }
}
void AdvancedNTPServer::setStratum(uint8_t st) {
    _stratum = (st <= NTP_STRATUM_MAX) ? st : NTP_STRATUM_UNSYNC;
    _updateRefTime();
}
void AdvancedNTPServer::setRefID(const char *id) {
    memset(&_refId, 0, sizeof(_refId));
    if (id)
        strncpy(_refId.str, id, 4);
}
void AdvancedNTPServer::setRootDelay(double s) { _rootDelay = _secondsToShort(s); }
void AdvancedNTPServer::setRootDispersion(double s) { _rootDispersion = _secondsToShort(s); }
void AdvancedNTPServer::setPrecision(int8_t p) { _precision = p; }

void AdvancedNTPServer::setPollLimits(int8_t minPoll, int8_t maxPoll) {
    if (minPoll > maxPoll)
        return;
    _pollMin = minPoll;
    _pollMax = maxPoll;
}

int8_t AdvancedNTPServer::measurePrecision() {
    if (!_cbUTC || !_cbSubSec)
        return _precision;

    // NOTE: this measures the latency of calling the UTC/sub-second
    // callbacks, used as a practical stand-in for system clock
    // resolution. It is an approximation, not a true clock-resolution
    // measurement — if your callbacks are unusually slow/fast relative to
    // the underlying clock hardware, set precision manually instead via
    // setPrecision().
    const uint32_t n = 2048;
    uint32_t start = micros();

    for (uint32_t i = 0; i < n; i++) {
        (void)_cbUTC();
        (void)_cbSubSec();
    }

    uint32_t end = micros();
    uint32_t elapsed = end - start;
    double avg = (double)elapsed / (1000000.0 * n);

    if (avg <= 0)
        avg = 1e-9;
    _precision = (int8_t)floor(log2(avg));

    if (_precision < -30)
        _precision = -30;
    if (_precision > -6)
        _precision = -6;

    NTP_LOGI("Precision measured: %d (%.3f us)", _precision, avg * 1e6);
    return _precision;
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setAuthEnabled(bool en) { _authEnabled = en; }
void AdvancedNTPServer::setAuthCallback(ntp_auth_cb_t cb) { _cbAuth = cb; }

bool AdvancedNTPServer::_constantTimeEquals(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Extension Field / Legacy MAC Location (RFC 7822-aware, v4.0.0)
// ---------------------------------------------------------------------------
// The pre-3.5.0 code assumed the bytes right after the 48-byte header were
// always exactly [KeyID:4][Digest:N] (the legacy MAC format). That is true
// for a plain authenticated client, but breaks the moment a client also
// sends any modern RFC 7822 extension field before the MAC — the server
// would misread part of that extension field as the KeyID/digest. This
// walker tries the simple case first (extension area is *exactly* a legacy
// MAC), then falls back to skipping recognized-length TLV fields until
// what's left matches a legacy MAC. It never reads past extLen.
bool AdvancedNTPServer::_locateLegacyMAC(const uint8_t *ext, size_t extLen, size_t digestLen, const uint8_t **outMac,
                                          size_t *outMacLen) {
    const size_t macLen = 4 + digestLen;

    // Common case: nothing but the legacy MAC present.
    if (extLen == macLen) {
        *outMac = ext;
        *outMacLen = macLen;
        return true;
    }

    // Walk TLV extension fields: [type:2][length:2][value:length-4].
    size_t offset = 0;
    size_t remaining = extLen;
    while (remaining >= 4) {
        uint16_t flen = ((uint16_t)ext[offset + 2] << 8) | ext[offset + 3];

        // A malformed or hostile length must not walk us out of bounds.
        if (flen < 4 || flen > remaining)
            break;

        offset += flen;
        remaining -= flen;

        if (remaining == macLen) {
            *outMac = ext + offset;
            *outMacLen = macLen;
            return true;
        }
    }

    return false;
}

bool AdvancedNTPServer::verifyMAC(const uint8_t *packet, size_t packetLen, uint32_t keyId, const uint8_t *digest) {
    if (!_cbAuth)
        return false;

    uint8_t computed[NTP_AUTH_DIGEST_LEN];

    if (!_cbAuth(keyId, packet, packetLen, computed))
        return false;
    return _constantTimeEquals(computed, digest, NTP_AUTH_DIGEST_LEN);
}

// ---------------------------------------------------------------------------
// Multicast
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setMulticastEnabled(bool en, uint32_t intervalSec) {
    _multicastEnabled = en;
    _multicastIntervalMs = intervalSec * 1000;

    if (_multicastIntervalMs < 1000)
        _multicastIntervalMs = 1000;
}

void AdvancedNTPServer::sendBroadcastPacket() {
    if (!_running || !_multicastEnabled)
        return;
    _sendBroadcastPacketInternal();
}

// ---------------------------------------------------------------------------
// Rate Limiting
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setRateLimitEnabled(bool en) { _rateLimitEnabled = en; }

void AdvancedNTPServer::setRateLimitParams(uint32_t rate, uint32_t burst) {
    _rateLimitRate = rate ? rate : 1;
    _rateLimitBurst = burst ? burst : 4;
}

void AdvancedNTPServer::clearRateLimitTable() {
    NTP_GUARD(_mux);
    memset(_tokenBuckets, 0, sizeof(_tokenBuckets));
}

// ---------------------------------------------------------------------------
// Leap Second
// ---------------------------------------------------------------------------
void AdvancedNTPServer::addLeapEvent(uint32_t unixTs, int8_t type) {
    NTP_GUARD(_mux);
    for (int i = 0; i < NTP_MAX_LEAP_EVENTS; i++) {
        if (!_leapEvents[i].active) {
            _leapEvents[i].unixTimestamp = unixTs;
            _leapEvents[i].type = (type > 0) ? 1 : (type < 0) ? 0xFF : 0;
            _leapEvents[i].active = true;

            NTP_LOGI("Leap event added at %lu (type=%d)", (unsigned long)unixTs, (int)type);
            return;
        }
    }

    NTP_LOGW("Leap event table full");
}

void AdvancedNTPServer::clearLeapEvents() {
    NTP_GUARD(_mux);
    memset(_leapEvents, 0, sizeof(_leapEvents));
}

uint8_t AdvancedNTPServer::checkLeapSecond(uint32_t currentUnixTime) {
    // Compared in 64-bit Unix seconds, not in wrapped 32-bit NTP seconds:
    // the NTP seconds field rolls over on 2036-02-07 (era 1), Unix seconds
    // held in 32 bits last until 2106.
    const int64_t now = (int64_t)currentUnixTime;
    NTP_GUARD(_mux);
    for (int i = 0; i < NTP_MAX_LEAP_EVENTS; i++) {
        if (!_leapEvents[i].active)
            continue;

        const int64_t event = (int64_t)_leapEvents[i].unixTimestamp;
        if (now >= event - 86400 && now < event) {
            return (_leapEvents[i].type == 1) ? NTP_LI_ADD_SECOND : NTP_LI_DEL_SECOND;
        }
    }

    return NTP_LI_NO_WARNING;
}

// ---------------------------------------------------------------------------
// Temperature Compensation
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setTemperatureCompensation(float ppmPerC, float refTempC) {
    _tempPPMperC = ppmPerC;
    _tempRefC = refTempC;
    _tempCompEnabled = (ppmPerC != 0.0f);
    // Reset the integration window so a stale elapsed-time base (from
    // before this call) doesn't get attributed to the new PPM value.
    _tempCompLastMs = 0;
    _tempCompAccumUs = 0.0;
}
void AdvancedNTPServer::setTempCallback(ntp_temp_cb_t cb) { _cbTemp = cb; }

// ---------------------------------------------------------------------------
// Interleaved Mode (v4.0.0: fully automatic and per-client — state lives in
// ntp_association_t::peerTxTimestamp, maintained inside _processPacket)
// ---------------------------------------------------------------------------
void AdvancedNTPServer::setInterleavedEnabled(bool en) { _interleavedEnabled = en; }

// ---------------------------------------------------------------------------
// Response Quality Filter (math derived from RFC 5905 A.5.1.1 / A.5.2)
//
// IMPORTANT: this is NOT the RFC 5905 client-side clock filter. That
// algorithm requires T4 (the time the client received the server's reply),
// which a server structurally never learns. What this filter actually
// smooths is the server-observed one-way gap between the client's claimed
// transmit time and the server's own receive time — useful as a per-client
// jitter/spike/quality diagnostic, but it conflates network delay with
// clock offset and must not be read as "the client's true clock offset".
// See README §6 for details and for how to get a real offset (compute it
// on the client, the normal NTP way).
// ---------------------------------------------------------------------------
#if NTP_ENABLE_CLOCK_FILTER

void AdvancedNTPServer::setClockFilterEnabled(bool enabled) { _clockFilterEnabled = enabled; }

void AdvancedNTPServer::_initClockFilter(int assocIdx) {
    if (assocIdx < 0 || assocIdx >= NTP_MAX_ASSOCIATIONS)
        return;

    ntp_clock_filter_t *cf = &_clockFilters[assocIdx];
    memset(cf, 0, sizeof(ntp_clock_filter_t));

    for (int i = 0; i < NTP_CF_NSTAGE; i++) {
        cf->stage[i].offset = 0.0;
        cf->stage[i].delay = NTP_CF_MAXDISP;
        cf->stage[i].dispersion = NTP_CF_MAXDISP;
        cf->stage[i].time = 0.0;
    }

    cf->peerOffset = 0.0;
    cf->peerDelay = NTP_CF_MAXDISP;
    cf->peerDispersion = NTP_CF_MAXDISP;
    cf->peerJitter = 0.0;
    cf->lastUpdateTime = 0.0;
    cf->validStages = 0;
}

void AdvancedNTPServer::_clockFilter(int assocIdx, double offset, double delay, double disp) {
    if (assocIdx < 0 || assocIdx >= NTP_MAX_ASSOCIATIONS)
        return;
    if (!_clockFilterEnabled)
        return;

    ntp_clock_filter_t *cf = &_clockFilters[assocIdx];
    double currentTime = millis() / 1000.0;
    ntp_cf_stage_t temp[NTP_CF_NSTAGE];
    double dtemp = cf->peerOffset;

    // Step 1: Shift register left, increase dispersion, copy to temp list
    for (int i = NTP_CF_NSTAGE - 1; i > 0; i--) {
        cf->stage[i] = cf->stage[i - 1];
        cf->stage[i].dispersion += NTP_CF_PHI * (currentTime - cf->lastUpdateTime);
        temp[i] = cf->stage[i];
    }

    // Insert new sample at stage 0 (youngest)
    cf->stage[0].time = currentTime;
    cf->stage[0].offset = offset;
    cf->stage[0].delay = delay;
    cf->stage[0].dispersion = disp;
    temp[0] = cf->stage[0];

    // Count valid stages
    uint8_t validCount = 0;
    for (int i = 0; i < NTP_CF_NSTAGE; i++) {
        if (cf->stage[i].delay < NTP_CF_MAXDISP - 0.001) {
            validCount++;
        }
    }
    cf->validStages = validCount;

    // Step 2: Sort temp list by increasing delay (insertion sort, N=8 —
    // faster than the previous bubble sort for a mostly-ordered shift
    // register and easier to keep branch-predictable on embedded cores)
    for (int i = 1; i < NTP_CF_NSTAGE; i++) {
        ntp_cf_stage_t key = temp[i];
        int j = i - 1;
        while (j >= 0 && temp[j].delay > key.delay) {
            temp[j + 1] = temp[j];
            j--;
        }
        temp[j + 1] = key;
    }

    // Step 3: Select best sample (lowest delay after sort)
    cf->peerOffset = temp[0].offset;
    cf->peerDelay = temp[0].delay;

    // Step 4: Compute peer dispersion as weighted sum
    // epsilon = sum(epsilon_i / 2^(i+1)) for i=0 to n-1
    double dispersion = 0.0;
    double jitterSum = 0.0;
    for (int i = 0; i < NTP_CF_NSTAGE; i++) {
        dispersion += temp[i].dispersion / (double)(1 << (i + 1));
        double diff = temp[i].offset - temp[0].offset;
        jitterSum += diff * diff;
    }
    cf->peerDispersion = dispersion;

    // Step 5: Compute jitter (RMS)
    int validForJitter = (validCount > 1) ? (validCount - 1) : 1;
    double jitter = sqrt(jitterSum / validForJitter);
    double sysPrecision = pow(2.0, _precision);
    if (jitter < sysPrecision)
        jitter = sysPrecision;
    cf->peerJitter = jitter;

    // Step 6: Spike detector (SGATE)
    bool spikeDetected = false;
    if (cf->lastUpdateTime > 0.0) {
        double deltaOffset = fabs(cf->peerOffset - dtemp);
        if (deltaOffset > NTP_CF_SGATE * cf->peerJitter) {
            double timeSinceLast = currentTime - cf->lastUpdateTime;
            if (timeSinceLast < 2.0 * 64.0) {
                spikeDetected = true;
                NTP_LOGW("RQF: spike detected on assoc %d (delta=%.6f, jitter=%.6f)", assocIdx, deltaOffset,
                         cf->peerJitter);
                cf->peerOffset = dtemp;
            }
        }
    }

    if (!spikeDetected) {
        cf->lastUpdateTime = currentTime;
    }

    // Update association with filtered values
    _associations[assocIdx].filteredOffsetSec = cf->peerOffset;
    _associations[assocIdx].filteredDelaySec = cf->peerDelay;
    _associations[assocIdx].filteredDispersion = cf->peerDispersion;
    _associations[assocIdx].filteredJitter = cf->peerJitter;
    _associations[assocIdx].filterValid = cf->validStages;

    NTP_LOGD("RQF assoc=%d: offset=%.6f delay=%.6f disp=%.6f jitter=%.6f valid=%d", assocIdx, cf->peerOffset,
             cf->peerDelay, cf->peerDispersion, cf->peerJitter, cf->validStages);
}

/// Compute root distance: lambda = (delta / 2) + epsilon + PHI * (t - t0)
double AdvancedNTPServer::_rootDistance(int assocIdx) const {
    if (assocIdx < 0 || assocIdx >= NTP_MAX_ASSOCIATIONS)
        return NTP_CF_MAXDISP;
    const ntp_clock_filter_t *cf = &_clockFilters[assocIdx];
    double t = millis() / 1000.0;
    double lambda = (cf->peerDelay / 2.0) + cf->peerDispersion + NTP_CF_PHI * (t - cf->lastUpdateTime);
    return lambda;
}

/// fit() — check if association is suitable for synchronization
bool AdvancedNTPServer::_fitAssociation(int assocIdx) const {
    if (assocIdx < 0 || assocIdx >= NTP_MAX_ASSOCIATIONS)
        return false;
    if (!_associations[assocIdx].active)
        return false;
    if (_stratum >= NTP_STRATUM_UNSYNC)
        return false;
    double rootDist = _rootDistance(assocIdx);
    if (rootDist > 1.5)
        return false;
    if (_clockFilters[assocIdx].validStages == 0)
        return false;
    return true;
}

bool AdvancedNTPServer::getClockFilterState(int assocIndex, ntp_clock_filter_t &out) const {
    if (assocIndex < 0 || assocIndex >= NTP_MAX_ASSOCIATIONS)
        return false;
    NTP_GUARD(_mux);
    if (!_associations[assocIndex].active)
        return false;
    out = _clockFilters[assocIndex];
    return true;
}

void AdvancedNTPServer::clearClockFilter(int assocIndex) {
    if (assocIndex >= 0 && assocIndex < NTP_MAX_ASSOCIATIONS) {
        NTP_GUARD(_mux);
        _initClockFilter(assocIndex);
    }
}

void AdvancedNTPServer::getFilterStats(int assocIndex, double &offsetSec, double &delaySec, double &dispersion,
                                       double &jitterSec, uint8_t &validStages) const {
    NTP_GUARD(_mux);
    if (assocIndex < 0 || assocIndex >= NTP_MAX_ASSOCIATIONS || !_associations[assocIndex].active) {
        offsetSec = delaySec = dispersion = jitterSec = 0.0;
        validStages = 0;
        return;
    }
    offsetSec = _associations[assocIndex].filteredOffsetSec;
    delaySec = _associations[assocIndex].filteredDelaySec;
    dispersion = _associations[assocIndex].filteredDispersion;
    jitterSec = _associations[assocIndex].filteredJitter;
    validStages = _associations[assocIndex].filterValid;
}

#endif // NTP_ENABLE_CLOCK_FILTER

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
ntp_statistics_t AdvancedNTPServer::getStatistics() const {
    NTP_GUARD(_mux);
    ntp_statistics_t s = _stats;
    s.uptimeSeconds = (millis() - _startTimeMs) / 1000;
    s.clockQuality = _clockQuality;
    return s;
}

void AdvancedNTPServer::resetStatistics() {
    NTP_GUARD(_mux);
    memset(&_stats, 0, sizeof(_stats));
    _startTimeMs = millis();
}

void AdvancedNTPServer::getAssociations(ntp_association_t *out, size_t maxCount, size_t &returnedCount) const {
    NTP_GUARD(_mux);
    returnedCount = 0;
    for (int i = 0; i < NTP_MAX_ASSOCIATIONS && returnedCount < maxCount; i++) {
        if (_associations[i].active) {
            out[returnedCount++] = _associations[i];
        }
    }
}

void AdvancedNTPServer::clearAssociations() {
    NTP_GUARD(_mux);
    memset(_associations, 0, sizeof(_associations));
#if NTP_ENABLE_CLOCK_FILTER
    for (int i = 0; i < NTP_MAX_ASSOCIATIONS; i++) {
        _initClockFilter(i);
    }
#endif
}

// ---------------------------------------------------------------------------
// Self-Test (v4.0.0)
// ---------------------------------------------------------------------------
// A cheap, network-free consistency check suitable for periodic use from a
// watchdog/health-check task. It does not validate time accuracy (it has
// no independent reference) — only internal wiring and invariants.
ntp_selftest_result_t AdvancedNTPServer::selfTest() const {
    ntp_selftest_result_t r;
    memset(&r, 0, sizeof(r));

    r.callbacksValid = (_cbUTC != nullptr) && (_cbSubSec != nullptr);
    r.precisionInRange = (_precision >= -30 && _precision <= -6);

    NTP_GUARD(_mux);

    // Association table: no duplicate active IPs, no active IP == 0.
    r.associationTableConsistent = true;
    for (int i = 0; i < NTP_MAX_ASSOCIATIONS && r.associationTableConsistent; i++) {
        if (!_associations[i].active)
            continue;
        if (_associations[i].ip == 0) {
            r.associationTableConsistent = false;
            break;
        }
        for (int j = i + 1; j < NTP_MAX_ASSOCIATIONS; j++) {
            if (_associations[j].active && _associations[j].ip == _associations[i].ip) {
                r.associationTableConsistent = false;
                break;
            }
        }
    }

    // Token bucket table: same duplicate-IP check, and tokens must never
    // exceed the configured burst size.
    r.tokenBucketTableConsistent = true;
    for (int i = 0; i < NTP_MAX_ASSOCIATIONS && r.tokenBucketTableConsistent; i++) {
        if (!_tokenBuckets[i].active)
            continue;
        if (_tokenBuckets[i].tokens > (float)_rateLimitBurst + 0.01f) {
            r.tokenBucketTableConsistent = false;
            break;
        }
        for (int j = i + 1; j < NTP_MAX_ASSOCIATIONS; j++) {
            if (_tokenBuckets[j].active && _tokenBuckets[j].ip == _tokenBuckets[i].ip) {
                r.tokenBucketTableConsistent = false;
                break;
            }
        }
    }

    r.ok = r.callbacksValid && r.precisionInRange && r.associationTableConsistent && r.tokenBucketTableConsistent;

    if (r.ok) {
        snprintf(r.message, sizeof(r.message), "OK (v%s)", NTP_SERVER_VERSION_STRING);
    } else {
        snprintf(r.message, sizeof(r.message), "FAIL: cb=%d prec=%d assoc=%d bucket=%d", r.callbacksValid,
                  r.precisionInRange, r.associationTableConsistent, r.tokenBucketTableConsistent);
    }

    return r;
}

// ---------------------------------------------------------------------------
// Config Persistence
// ---------------------------------------------------------------------------
void AdvancedNTPServer::exportConfig(ntp_config_t &cfg) const {
    cfg.version = _version;
    cfg.stratum = _stratum;
    cfg.li = _li;
    memcpy(cfg.refId, _refId.str, 4);
    cfg.precision = _precision;
    cfg.rootDelaySec = _shortToSeconds(_rootDelay);
    cfg.rootDispersionSec = _shortToSeconds(_rootDispersion);
    cfg.rateLimitBurst = _rateLimitBurst;
    cfg.rateLimitRate = _rateLimitRate;
    cfg.authEnabled = _authEnabled;
    cfg.multicastEnabled = _multicastEnabled;
    cfg.multicastIntervalSec = _multicastIntervalMs / 1000;
    cfg.temperatureCompEnabled = _tempCompEnabled;
#if NTP_ENABLE_CLOCK_FILTER
    cfg.clockFilterEnabled = _clockFilterEnabled;
#else
    cfg.clockFilterEnabled = false;
#endif
}

void AdvancedNTPServer::importConfig(const ntp_config_t &cfg) {
    // Values come from user storage (EEPROM/flash/JSON) and may be corrupt,
    // so every field is range-checked instead of being trusted blindly.
    _version = (cfg.version >= 1 && cfg.version <= 4) ? cfg.version : 4;
    _stratum = (cfg.stratum <= NTP_STRATUM_UNSYNC) ? cfg.stratum : NTP_STRATUM_UNSYNC;
    _li = (cfg.li <= NTP_LI_NOT_IN_SYNC) ? cfg.li : NTP_LI_NOT_IN_SYNC;
    memcpy(_refId.str, cfg.refId, 4);
    _precision = cfg.precision;
    if (_precision < -30)
        _precision = -30;
    if (_precision > -6)
        _precision = -6;
    setRootDelay(cfg.rootDelaySec);
    setRootDispersion(cfg.rootDispersionSec);
    setRateLimitParams(cfg.rateLimitRate, cfg.rateLimitBurst);
    _authEnabled = cfg.authEnabled;
    _multicastEnabled = cfg.multicastEnabled;
    setMulticastEnabled(cfg.multicastEnabled, cfg.multicastIntervalSec);
    _tempCompEnabled = cfg.temperatureCompEnabled;
#if NTP_ENABLE_CLOCK_FILTER
    _clockFilterEnabled = cfg.clockFilterEnabled;
#endif
}

// ---------------------------------------------------------------------------
// Syslog
// ---------------------------------------------------------------------------
#if NTP_ENABLE_SYSLOG
void AdvancedNTPServer::setSyslogTarget(IPAddress ip, uint16_t port) {
    _syslogIP = ip;
    _syslogPort = port;
}
void AdvancedNTPServer::setSyslogCallback(ntp_syslog_cb_t cb) { _cbSyslog = cb; }
#endif

void AdvancedNTPServer::_logSyslog(const char *fmt, ...) {
#if !NTP_ENABLE_SYSLOG
    (void)fmt;
#else
    if (!_cbSyslog)
        return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    _cbSyslog(buf);
#endif
}

// ---------------------------------------------------------------------------
// Packet Processing
// ---------------------------------------------------------------------------
void AdvancedNTPServer::_processPacket(AsyncUDPPacket &packet) {
    if (_suspended)
        return;
    if (!_cbUTC || !_cbSubSec) {
        NTP_LOGE("Null callbacks");
        return;
    }

    size_t len = packet.length();
    if (len < sizeof(ntp_packet_t)) {
        NTP_GUARD(_mux);
        _stats.packetsReceived++;
        _stats.packetsInvalid++;
        return;
    }

    {
        NTP_GUARD(_mux);
        _stats.packetsReceived++;
    }

    uint32_t clientIP = packet.remoteIP();

    // Rate limit. The decision is taken under the lock, but the reply is
    // sent *after* releasing it: AsyncUDP::write() allocates and takes the
    // TCP/IP core lock, which must never happen with interrupts disabled.
    if (_rateLimitEnabled) {
        bool allowed;
        {
            NTP_GUARD(_mux);
            allowed = _checkTokenBucket(clientIP);
            if (!allowed)
                _stats.packetsDropped++;
        }
        if (!allowed) {
            _sendKissOfDeath(packet, NTP_KOD_RATE);
            return;
        }
    }

    // Capture RX timestamp (T2)
    uint32_t rxUtc = _cbUTC();
    uint32_t rxUs = _cbSubSec();
    _applyTemperatureCompensation(rxUtc, rxUs);
    ntp_timestamp_t rxTs = _makeTimestamp(rxUtc, rxUs);

    // Parse incoming packet
    ntp_packet_t req;
    memcpy(&req, packet.data(), sizeof(ntp_packet_t));

    // NTOH
    req.rootDelay.raw = ntohl(req.rootDelay.raw);
    req.rootDispersion.raw = ntohl(req.rootDispersion.raw);
    // refId is an opaque 4-byte string/address on the wire: no byte swap.
    req.refTime.seconds = ntohl(req.refTime.seconds);
    req.refTime.fraction = ntohl(req.refTime.fraction);
    req.origTime.seconds = ntohl(req.origTime.seconds);
    req.origTime.fraction = ntohl(req.origTime.fraction);
    req.rxTime.seconds = ntohl(req.rxTime.seconds);
    req.rxTime.fraction = ntohl(req.rxTime.fraction);
    req.txTime.seconds = ntohl(req.txTime.seconds);
    req.txTime.fraction = ntohl(req.txTime.fraction);

    uint8_t clientMode = req.flags.mode;
    uint8_t clientVer = req.flags.version;

    if (clientVer < 1 || clientVer > 4) {
        NTP_GUARD(_mux);
        _stats.packetsInvalid++;
        return;
    }

    // Control Messages (Mode 6, ntpq) are not implemented. They are counted
    // and dropped without a reply: answering with a malformed mode-6 packet
    // would confuse ntpq, and a real reply is not worth the attack surface.
    if (clientMode == NTP_MODE_CONTROL) {
        NTP_GUARD(_mux);
        _stats.controlMsgsReceived++;
        return;
    }

    if (clientMode != NTP_MODE_CLIENT && clientMode != 1) {
        NTP_GUARD(_mux);
        _stats.packetsInvalid++;
        return;
    }

    // Authentication check
    size_t extLen = len - sizeof(ntp_packet_t);
    bool authenticated = false;
    uint32_t authKeyId = 0;
    if (_authEnabled) {
        const char *kod = nullptr;
        const uint8_t *rawData = (const uint8_t *)packet.data();

        if (extLen < (4 + NTP_AUTH_DIGEST_LEN)) {
            // No room for a MAC at all: reject explicitly rather than
            // silently accepting an unauthenticated packet.
            kod = NTP_KOD_AUTH;
        } else {
            const uint8_t *extStart = rawData + sizeof(ntp_packet_t);
            const uint8_t *macStart = nullptr;
            size_t macLen = 0;

            // Walk past any TLV extension fields to find the legacy MAC.
            if (!_locateLegacyMAC(extStart, extLen, NTP_AUTH_DIGEST_LEN, &macStart, &macLen)) {
                kod = NTP_KOD_CRYP;
            } else {
                // Key-ID: 4 bytes at macStart, network byte order.
                uint32_t keyId;
                memcpy(&keyId, macStart, sizeof(keyId));
                keyId = ntohl(keyId);

                // The digest covers the packet exactly as it appeared on
                // the wire (header + any extension fields before the MAC).
                // It must NOT be computed over `req`, whose multi-byte
                // fields were already converted to host byte order above.
                const size_t signedLen = (size_t)(macStart - rawData);
                if (!verifyMAC(rawData, signedLen, keyId, macStart + 4)) {
                    kod = NTP_KOD_AUTH;
                } else {
                    authenticated = true;
                    authKeyId = keyId;
                }
            }
        }

        if (kod) {
            {
                NTP_GUARD(_mux);
                _stats.packetsAuthFail++;
            }
            _sendKissOfDeath(packet, kod);
            return;
        }
    }

    // Build response
    ntp_packet_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.flags.li = _effectiveLI(rxUtc);
    resp.flags.version = clientVer;
    resp.flags.mode = NTP_MODE_SERVER;
    resp.stratum = (_stratum >= NTP_STRATUM_UNSYNC) ? NTP_STRATUM_UNSPECIFIED : _stratum;

    // Clamp the echoed poll interval to the server's configured bounds
    // (RFC 5905 §7.2) instead of blindly trusting the client's value.
    int8_t clampedPoll = req.pollInterval;
    if (clampedPoll < _pollMin)
        clampedPoll = _pollMin;
    if (clampedPoll > _pollMax)
        clampedPoll = _pollMax;
    resp.pollInterval = clampedPoll;

    resp.precision = _precision;
    resp.rootDelay = _rootDelay;
    resp.rootDispersion = _rootDispersion;
    resp.refId = _refId;
    resp.refTime = _refTime;
    resp.origTime = req.txTime;
    resp.rxTime = rxTs;

    // Look up (or create) this client's association now, under lock, so
    // Interleaved Mode can use *this specific client's* previous T3
    // instead of a single server-wide value (fixed in v4.0.0 — the prior
    // single global _peerTxTimestamp meant concurrent interleaved clients
    // would corrupt each other's origTime).
    int assocIdx;
    {
        NTP_GUARD(_mux);
        assocIdx = _findOrCreateAssociation(clientIP);
        if (_interleavedEnabled && assocIdx >= 0) {
            const ntp_timestamp_t &prevT3 = _associations[assocIdx].peerTxTimestamp;
            if (prevT3.seconds != 0 || prevT3.fraction != 0) {
                resp.origTime = prevT3;
            }
        }
    }

    uint32_t txUtc = _cbUTC();
    uint32_t txUs = _cbSubSec();
    _applyTemperatureCompensation(txUtc, txUs);
    resp.txTime = _makeTimestamp(txUtc, txUs);

    // Server-side diagnostic estimate only — see the Response Quality
    // Filter comment block above for what this is and is not.
    int32_t rawOffsetUs = _estimateOffset(req.txTime, rxTs, resp.txTime, resp.txTime);
    int32_t rawDelayUs = _estimateDelay(req.txTime, rxTs, resp.txTime, resp.txTime);

    // Update association (including this response's T3, for the *next*
    // interleaved exchange with this same client).
    {
        NTP_GUARD(_mux);
        // Re-validate: under extreme load with a full association table,
        // another packet could have evicted this slot for a different
        // client between the lock above and this one. If so, skip rather
        // than corrupting a different client's record.
        if (assocIdx >= 0 && _associations[assocIdx].active && _associations[assocIdx].ip == clientIP) {
            _updateAssociation(assocIdx, req, rawOffsetUs, rawDelayUs);
            _associations[assocIdx].peerTxTimestamp = resp.txTime;

#if NTP_ENABLE_CLOCK_FILTER
            if (_clockFilterEnabled) {
                double offsetSec = rawOffsetUs / 1000000.0;
                double delaySec = rawDelayUs / 1000000.0;
                double clientPrec = (req.precision < 0) ? pow(2.0, req.precision) : 1e-6;
                double serverPrec = (_precision < 0) ? pow(2.0, _precision) : 1e-6;
                double disp = clientPrec + serverPrec + NTP_CF_PHI * delaySec;
                _clockFilter(assocIdx, offsetSec, delaySec, disp);
            }
#endif
        }
    }

    // Convert to network byte order
    resp.rootDelay.raw = htonl(resp.rootDelay.raw);
    resp.rootDispersion.raw = htonl(resp.rootDispersion.raw);
    resp.refTime.seconds = htonl(resp.refTime.seconds);
    resp.refTime.fraction = htonl(resp.refTime.fraction);
    resp.origTime.seconds = htonl(resp.origTime.seconds);
    resp.origTime.fraction = htonl(resp.origTime.fraction);
    resp.rxTime.seconds = htonl(resp.rxTime.seconds);
    resp.rxTime.fraction = htonl(resp.rxTime.fraction);
    resp.txTime.seconds = htonl(resp.txTime.seconds);
    resp.txTime.fraction = htonl(resp.txTime.fraction);

    // When the client authenticated, sign the reply with the same key so
    // that clients configured with that key accept it (RFC 5905 section 7.3).
    uint8_t txBuf[sizeof(ntp_packet_t) + 4 + NTP_AUTH_DIGEST_LEN];
    size_t txLen = sizeof(resp);
    memcpy(txBuf, &resp, sizeof(resp));
    if (authenticated && _cbAuth) {
        uint8_t digest[NTP_AUTH_DIGEST_LEN];
        if (_cbAuth(authKeyId, txBuf, sizeof(resp), digest)) {
            uint32_t keyIdNet = htonl(authKeyId);
            memcpy(txBuf + txLen, &keyIdNet, 4);
            memcpy(txBuf + txLen + 4, digest, NTP_AUTH_DIGEST_LEN);
            txLen += 4 + NTP_AUTH_DIGEST_LEN;
        } else {
            NTP_LOGW("Auth callback failed to sign reply for key %lu", (unsigned long)authKeyId);
        }
    }
    packet.write(txBuf, txLen);

    {
        NTP_GUARD(_mux);
        _stats.packetsSent++;
        _stats.lastClientIP = clientIP;
    }

    // Periodic tasks
    _updateDrift();
    if (_multicastEnabled && (millis() - _lastMulticastMs) > _multicastIntervalMs) {
        _lastMulticastMs = millis();
        _sendBroadcastPacketInternal();
    }
}

// ---------------------------------------------------------------------------
// Kiss-o'-Death
// ---------------------------------------------------------------------------
void AdvancedNTPServer::_sendKissOfDeath(AsyncUDPPacket &packet, const char *code) {
    const uint8_t *rq = (const uint8_t *)packet.data();
    const size_t rqLen = packet.length();

    // Mirror the client's version, and echo its transmit timestamp (bytes
    // 40..47 of the request, still in network order) as origTime. RFC 5905
    // clients discard a reply whose origin timestamp does not match what
    // they sent, so a KoD without this echo is silently ignored.
    uint8_t ver = (rqLen >= 1) ? ((rq[0] >> 3) & 0x07) : 4;
    if (ver < 1 || ver > 4)
        ver = 4;

    ntp_packet_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.flags.li = NTP_LI_NOT_IN_SYNC;
    resp.flags.version = ver;
    resp.flags.mode = NTP_MODE_SERVER;
    resp.stratum = 0;
    resp.precision = _precision;
    resp.rootDelay = _secondsToShort(1.0);
    resp.rootDispersion = _secondsToShort(NTP_CF_MAXDISP);
    memset(&resp.refId, 0, sizeof(resp.refId));
    strncpy(resp.refId.str, code, 4);

    resp.rootDelay.raw = htonl(resp.rootDelay.raw);
    resp.rootDispersion.raw = htonl(resp.rootDispersion.raw);
    if (rqLen >= sizeof(ntp_packet_t))
        memcpy(&resp.origTime, rq + offsetof(ntp_packet_t, txTime), sizeof(resp.origTime));

    packet.write((uint8_t *)&resp, sizeof(resp));

    NTP_GUARD(_mux);
    _stats.kissOfDeathSent++;
}

// ---------------------------------------------------------------------------
// Broadcast / Multicast
// ---------------------------------------------------------------------------
void AdvancedNTPServer::_sendBroadcastPacketInternal() {
    if (!_running)
        return;
    ntp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.flags.li = NTP_LI_NO_WARNING; // set below once the timestamp is known
    pkt.flags.version = 4;
    pkt.flags.mode = NTP_MODE_BROADCAST;
    pkt.stratum = (_stratum >= NTP_STRATUM_UNSYNC) ? 0 : _stratum;
    pkt.pollInterval = 6;
    pkt.precision = _precision;
    pkt.rootDelay = _rootDelay;
    pkt.rootDispersion = _rootDispersion;
    pkt.refId = _refId;
    pkt.refTime = _refTime;

    uint32_t utc = _cbUTC();
    uint32_t us = _cbSubSec();
    _applyTemperatureCompensation(utc, us);
    pkt.txTime = _makeTimestamp(utc, us);
    pkt.flags.li = _effectiveLI(utc);

    pkt.rootDelay.raw = htonl(pkt.rootDelay.raw);
    pkt.rootDispersion.raw = htonl(pkt.rootDispersion.raw);
    pkt.refTime.seconds = htonl(pkt.refTime.seconds);
    pkt.refTime.fraction = htonl(pkt.refTime.fraction);
    pkt.txTime.seconds = htonl(pkt.txTime.seconds);
    pkt.txTime.fraction = htonl(pkt.txTime.fraction);

    _udpMulticast.writeTo((uint8_t *)&pkt, sizeof(pkt), IPAddress(224, 0, 1, 1), NTP_SRV_DEFAULT_PORT);

    NTP_GUARD(_mux);
    _stats.broadcastSent++;
}

// ---------------------------------------------------------------------------
// Token Bucket Rate Limiting
// NOTE: caller (_processPacket) already holds _mux when calling this.
// ---------------------------------------------------------------------------
bool AdvancedNTPServer::_checkTokenBucket(uint32_t clientIP) {
    uint32_t now = millis();
    int emptySlot = -1;
    int oldestIdx = -1;
    uint32_t oldestTime = 0xFFFFFFFF;

    for (int i = 0; i < NTP_MAX_ASSOCIATIONS; i++) {
        if (!_tokenBuckets[i].active) {
            if (emptySlot < 0)
                emptySlot = i;
            continue;
        }
        if (_tokenBuckets[i].ip == clientIP) {
            float elapsedSec = (now - _tokenBuckets[i].lastUpdateMs) / 1000.0f;
            _tokenBuckets[i].tokens += elapsedSec * _rateLimitRate;
            if (_tokenBuckets[i].tokens > _rateLimitBurst)
                _tokenBuckets[i].tokens = _rateLimitBurst;
            _tokenBuckets[i].lastUpdateMs = now;

            if (_tokenBuckets[i].tokens >= 1.0f) {
                _tokenBuckets[i].tokens -= 1.0f;
                return true;
            } else {
                _tokenBuckets[i].violations++;
                return false;
            }
        }
        if (_tokenBuckets[i].lastUpdateMs < oldestTime) {
            oldestTime = _tokenBuckets[i].lastUpdateMs;
            oldestIdx = i;
        }
    }

    int idx = (emptySlot >= 0) ? emptySlot : oldestIdx;
    if (idx >= 0) {
        _tokenBuckets[idx].active = true;
        _tokenBuckets[idx].ip = clientIP;
        _tokenBuckets[idx].lastUpdateMs = now;
        _tokenBuckets[idx].tokens = _rateLimitBurst - 1.0f;
        _tokenBuckets[idx].violations = 0;
        _tokenBuckets[idx].kodSent = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Association Table
// NOTE: caller (_processPacket) already holds _mux when calling these.
// ---------------------------------------------------------------------------
int AdvancedNTPServer::_findOrCreateAssociation(uint32_t ip) {
    int empty = -1;
    int oldest = -1;
    uint32_t oldestTime = 0xFFFFFFFF;

    for (int i = 0; i < NTP_MAX_ASSOCIATIONS; i++) {
        if (!_associations[i].active) {
            if (empty < 0)
                empty = i;
            continue;
        }
        if (_associations[i].ip == ip)
            return i;
        if (_associations[i].lastSeenMs < oldestTime) {
            oldestTime = _associations[i].lastSeenMs;
            oldest = i;
        }
    }
    int idx = (empty >= 0) ? empty : oldest;
    if (idx >= 0) {
        memset(&_associations[idx], 0, sizeof(ntp_association_t));
        _associations[idx].active = true;
        _associations[idx].ip = ip;
#if NTP_ENABLE_CLOCK_FILTER
        _initClockFilter(idx);
#endif
    }
    return idx;
}

void AdvancedNTPServer::_updateAssociation(int idx, const ntp_packet_t &req, int32_t offsetUs, int32_t delayUs) {
    if (idx < 0 || idx >= NTP_MAX_ASSOCIATIONS)
        return;
    _associations[idx].lastSeenMs = millis();
    _associations[idx].packetsReceived++;
    _associations[idx].packetsSent++;
    _associations[idx].version = req.flags.version;
    _associations[idx].mode = req.flags.mode;
    _associations[idx].lastPoll = req.pollInterval;
    _associations[idx].lastOffsetUs = offsetUs;
    _associations[idx].lastDelayUs = delayUs;
}

// ---------------------------------------------------------------------------
// Drift Estimation
// ---------------------------------------------------------------------------
// Compares how far the UTC callbacks advanced against how far millis()
// advanced over the same window (>= 60 s). Only meaningful when the UTC
// source runs independently of millis(); a step correction applied to the
// UTC source inside the window shows up as a one-off spike.
void AdvancedNTPServer::_updateDrift() {
    uint32_t now = millis();
    uint32_t wallMs = now - _lastDriftCheckMs;
    if (wallMs < 60000)
        return;

    uint32_t utc = _cbUTC();
    uint32_t us = _cbSubSec();

    int64_t wallUs = (int64_t)wallMs * 1000LL;
    int64_t clockUs = ((int64_t)utc - (int64_t)_lastDriftUTC) * 1000000LL + ((int64_t)us - (int64_t)_lastDriftUs);
    float ppm = (float)((double)(clockUs - wallUs) * 1e6 / (double)wallUs);

    _lastDriftCheckMs = now;
    _lastDriftUTC = utc;
    _lastDriftUs = us;

    NTP_GUARD(_mux);
    _stats.estimatedDriftPPM = ppm;
}

// ---------------------------------------------------------------------------
// Reference Time Update
// ---------------------------------------------------------------------------
void AdvancedNTPServer::_updateRefTime() {
    if (!_cbUTC || !_cbSubSec)
        return;

    uint32_t utc = _cbUTC();
    uint32_t us = _cbSubSec();

    _refTime = _makeTimestamp(utc, us);
    _refTimeUTC = utc;
}

// ---------------------------------------------------------------------------
// Temperature Compensation (v4.0.0: actually applies a correction)
// ---------------------------------------------------------------------------
// Model: a crystal running `deltaPPM` fast/slow accumulates
// `deltaPPM * elapsedSeconds` microseconds of error since the last time we
// corrected. We track elapsed wall-clock time between calls (via millis())
// and fold the resulting correction into (utcSec, us), carrying between the
// two on overflow/underflow. A running fractional-microsecond accumulator
// avoids losing small corrections to integer rounding when called every
// packet (which can be faster than 1 correction-microsecond apart).
void AdvancedNTPServer::_applyTemperatureCompensation(uint32_t &utcSec, uint32_t &us) {
    if (!_tempCompEnabled || !_cbTemp)
        return;

    uint32_t now = millis();
    if (_tempCompLastMs == 0) {
        // First call: nothing to integrate over yet.
        _tempCompLastMs = now;
        return;
    }

    float temp = _cbTemp();
    float deltaPPM = (temp - _tempRefC) * _tempPPMperC;
    double elapsedSec = (now - _tempCompLastMs) / 1000.0;
    _tempCompLastMs = now;

    if (fabsf(deltaPPM) < 0.001f || elapsedSec <= 0.0)
        return;

    // ppm = us of error per second, so ppm * elapsedSec = us of error.
    _tempCompAccumUs += (double)deltaPPM * elapsedSec;

    int32_t wholeUs = (int32_t)_tempCompAccumUs; // truncate toward zero
    if (wholeUs == 0)
        return; // not enough accumulated yet — keep the fractional part
    _tempCompAccumUs -= wholeUs;

    _foldMicroseconds(wholeUs, utcSec, us);
}

void AdvancedNTPServer::_foldMicroseconds(int64_t deltaUs, uint32_t &utcSec, uint32_t &us) {
    int64_t correctedUs = (int64_t)us + deltaUs;
    int64_t correctedSec = (int64_t)utcSec;

    // Carry between seconds and microseconds in either direction.
    while (correctedUs < 0) {
        correctedUs += 1000000;
        correctedSec -= 1;
    }
    while (correctedUs >= 1000000) {
        correctedUs -= 1000000;
        correctedSec += 1;
    }
    if (correctedSec < 0)
        correctedSec = 0; // never hand back a timestamp before the epoch

    utcSec = (uint32_t)correctedSec;
    us = (uint32_t)correctedUs;
}

// Leap indicator to advertise right now. A manually configured warning or
// "not in sync" state always wins. Otherwise the optional leap callback is
// authoritative if one was supplied; without it the leap-event table is used.
uint8_t AdvancedNTPServer::_effectiveLI(uint32_t utcSec) {
    if (_li != NTP_LI_NO_WARNING)
        return _li;
    if (_cbLeap) {
        uint8_t li = _cbLeap();
        return (li <= NTP_LI_NOT_IN_SYNC) ? li : (uint8_t)NTP_LI_NO_WARNING;
    }
    return checkLeapSecond(utcSec);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
ntp_timestamp_t AdvancedNTPServer::_makeTimestamp(uint32_t utcSec, uint32_t us) {
    ntp_timestamp_t ts;
    ts.seconds = utcSec + NTP_TIMESTAMP_DELTA;
    ts.fraction = _usToFraction(us);
    return ts;
}

ntp_short_t AdvancedNTPServer::_secondsToShort(double sec) {
    ntp_short_t s;
    if (sec < 0)
        sec = 0;
    if (sec > 65535.9999)
        sec = 65535.9999;
    uint32_t raw = (uint32_t)round(sec * NTP_SHORT_SCALE);
    s.raw = raw;
    return s;
}

uint32_t AdvancedNTPServer::_usToFraction(uint32_t us) {
    if (us > 999999)
        us = 999999;
    return (uint32_t)(((uint64_t)us * 4294967296ULL) / 1000000ULL);
}

double AdvancedNTPServer::_shortToSeconds(ntp_short_t s) { return (double)s.raw / NTP_SHORT_SCALE; }

// FIX (v3.5.0): each fraction field is a uint32_t. The previous code
// subtracted two uint32_t values first (which wraps modulo 2^32 on a
// negative result) and only then cast the wrapped result to int64_t,
// silently turning small negative differences into huge positive ones.
// Each operand is now cast to int64_t *before* subtracting.
// Era-safe: NTP seconds are a 32-bit counter that wraps in 2036, so two
// timestamps on opposite sides of the rollover differ by ~2^32 when
// subtracted as plain integers. Subtracting in uint32 and reinterpreting as
// int32 yields the true (small) difference as long as the two instants are
// less than 68 years apart, which is the standard NTP technique (RFC 5905 s6).
static inline int64_t ntpSecDiff(uint32_t a, uint32_t b) { return (int64_t)(int32_t)(a - b); }

int32_t AdvancedNTPServer::_estimateOffset(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2,
                                           const ntp_timestamp_t &t3, const ntp_timestamp_t &t4) {
    int64_t off = (ntpSecDiff(t2.seconds, t1.seconds) + ntpSecDiff(t3.seconds, t4.seconds)) * 1000000LL;
    off += ((int64_t)t2.fraction - (int64_t)t1.fraction + (int64_t)t3.fraction - (int64_t)t4.fraction) * 1000000LL /
           4294967296LL;
    return (int32_t)(off / 2);
}

int32_t AdvancedNTPServer::_estimateDelay(const ntp_timestamp_t &t1, const ntp_timestamp_t &t2,
                                          const ntp_timestamp_t &t3, const ntp_timestamp_t &t4) {
    int64_t d = (ntpSecDiff(t4.seconds, t1.seconds) - ntpSecDiff(t3.seconds, t2.seconds)) * 1000000LL;
    d += ((int64_t)t4.fraction - (int64_t)t1.fraction - (int64_t)t3.fraction + (int64_t)t2.fraction) * 1000000LL /
         4294967296LL;
    return (int32_t)d;
}
