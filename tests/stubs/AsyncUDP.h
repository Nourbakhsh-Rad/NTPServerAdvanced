#pragma once
#include "Arduino.h"
#include <functional>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

// Minimal stand-in for AsyncUDPPacket. Tests construct one with the raw
// request bytes and inspect everything the library wrote back via sent().
class AsyncUDPPacket {
  public:
    AsyncUDPPacket() {}
    AsyncUDPPacket(const uint8_t *d, size_t n, uint32_t ip) : _data(d, d + n), _ip(ip) {}
    size_t length() const { return _data.size(); }
    uint32_t remoteIP() const { return _ip; }
    const uint8_t *data() const { return _data.data(); }
    size_t write(const uint8_t *d, size_t n) {
        _sent.assign(d, d + n);
        _writes++;
        return n;
    }
    const std::vector<uint8_t> &sent() const { return _sent; }
    int writes() const { return _writes; }

  private:
    std::vector<uint8_t> _data;
    std::vector<uint8_t> _sent;
    uint32_t _ip = 0;
    int _writes = 0;
};

class AsyncUDP {
  public:
    bool listen(uint16_t) { return true; }
    bool listenMulticast(IPAddress, uint16_t) { return true; }
    void onPacket(std::function<void(AsyncUDPPacket)>) {}
    void close() {}
    void writeTo(const uint8_t *, size_t, IPAddress, uint16_t) {}
};
