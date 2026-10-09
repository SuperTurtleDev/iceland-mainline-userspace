/* Minimal SHA-256 (FIPS 180-4), used only to derive the keymaster
 * ROT / verified-boot-key digests from the configured AVB public key.
 * No external crypto dependency on purpose: the deb stays libc-only. */
#ifndef KM_SHA256_H
#define KM_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256_ctx {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);

/* one-shot */
void sha256(const void *data, size_t len, uint8_t out[32]);

#endif /* KM_SHA256_H */
