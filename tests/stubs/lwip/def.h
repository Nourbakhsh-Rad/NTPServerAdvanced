#pragma once
#include <cstdint>
inline uint32_t htonl(uint32_t x){ return __builtin_bswap32(x); }
inline uint32_t ntohl(uint32_t x){ return __builtin_bswap32(x); }
