#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <string>

// Tests can move the fake clock with fake_millis() = N.
inline uint32_t &fake_millis() {
    static uint32_t v = 0;
    return v;
}
inline uint32_t millis() { return fake_millis(); }
inline uint32_t micros() { return fake_millis() * 1000u; }
inline void delay(uint32_t) {}
inline void noInterrupts() {}
inline void interrupts() {}

struct SerialClass {
    void printf(const char* fmt, ...) { (void)fmt; }
    void println(const char* s = "") { (void)s; }
    void print(const char* s) { (void)s; }
    void begin(uint32_t) {}
};
extern SerialClass Serial;

class IPAddress {
public:
    IPAddress() {}
    IPAddress(uint32_t v) : _v(v) {}
    IPAddress(int a, int b, int c, int d) { _v = (a<<24)|(b<<16)|(c<<8)|d; }
    std::string toString() const { return std::string("0.0.0.0"); }
    uint32_t _v = 0;
};

using String = std::string;
