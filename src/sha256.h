/* C2 (C2-SPEC.md §2.2 HELLO plugin_sha, §2.3 modset2): SHA-256 (FIPS 180-4). Pure C. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t bl; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *p, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *p, size_t n, uint8_t out[32]);
/* the first 8 bytes of the digest as a big-endian u64 (the HELLO plugin_sha); 0 = unreadable */
uint64_t sha256_file_prefix64(const char *path);
