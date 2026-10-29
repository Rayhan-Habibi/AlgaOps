/**
 * @file ascon_hash256.h
 * @brief Ascon-Hash256 as specified in NIST SP 800-232 (August 2025).
 *
 * The permutation and the one-shot hash are taken from the Ascon reference
 * implementation (https://github.com/ascon/ascon-c, crypto_hash/asconhash256/ref,
 * commit 446347f, authors Christoph Dobraunig and Martin Schläffer, CC0 1.0).
 * The incremental init/update/final interface is a thin buffering wrapper
 * around the same absorb/squeeze steps; tests prove it equals the one-shot
 * reference for every split of the official KAT messages.
 */

#ifndef ASCON_HASH256_H
#define ASCON_HASH256_H

#include "algaops_export.h"

#include <stddef.h>
#include <stdint.h>

#define ASCON_HASH256_BYTES 32

typedef struct {
    uint64_t x[5];
    uint8_t  buf[8];
    size_t   buf_len;
} AsconHash256Ctx;

/** One-shot Ascon-Hash256 (reference code path). */
ALGAOPS_API void ascon_hash256(const uint8_t *in, size_t len,
                               uint8_t out[ASCON_HASH256_BYTES]);

ALGAOPS_API void ascon_hash256_init(AsconHash256Ctx *ctx);
ALGAOPS_API void ascon_hash256_update(AsconHash256Ctx *ctx, const uint8_t *in, size_t len);
ALGAOPS_API void ascon_hash256_final(AsconHash256Ctx *ctx, uint8_t out[ASCON_HASH256_BYTES]);

/** Lower-case hex encoding of a 32-byte digest (out must hold 65 chars). */
ALGAOPS_API void ascon_hash256_hex(const uint8_t digest[ASCON_HASH256_BYTES], char out[65]);

#endif /* ASCON_HASH256_H */
