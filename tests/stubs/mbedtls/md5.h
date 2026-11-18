#pragma once
struct mbedtls_md5_context { int dummy; };
inline void mbedtls_md5_init(mbedtls_md5_context*) {}
inline void mbedtls_md5_starts(mbedtls_md5_context*) {}
inline void mbedtls_md5_update(mbedtls_md5_context*, const unsigned char*, unsigned long) {}
inline void mbedtls_md5_finish(mbedtls_md5_context*, unsigned char*) {}
inline void mbedtls_md5_free(mbedtls_md5_context*) {}
