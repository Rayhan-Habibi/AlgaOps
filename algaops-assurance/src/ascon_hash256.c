/*
 * ascon_hash256.c — Ascon-Hash256 (NIST SP 800-232).
 *
 * Vendored from the Ascon reference implementation:
 *   https://github.com/ascon/ascon-c  crypto_hash/asconhash256/ref
 *   commit 446347f21b209f3921c65ece70027c366cbe1693 (2026-01-28)
 *   implementors: Christoph Dobraunig, Martin Schläffer
 *   license: CC0 1.0 Universal (public domain dedication)
 *
 * Changes from the reference: the files api.h/ascon.h/constants.h/word.h/
 * round.h/permutations.h/hash.c are merged into one translation unit, debug
 * printing is removed, integer types are widened to size_t, and an
 * incremental (init/update/final) interface is added on top of the same
 * absorb and squeeze steps.  The permutation, IV, padding and byte order are
 * unchanged.  Verified against the official LWC_HASH_KAT_128_256.txt vectors
 * (tests/test_ascon_hash256.c).
 */

#include "../include/ascon_hash256.h"

#include <string.h>

/* ---- constants.h (hash subset) ----------------------------------------- */
#define ASCON_HASH_VARIANT   2
#define ASCON_PA_ROUNDS      12
#define ASCON_HASH_PB_ROUNDS 12
#define ASCON_HASH_SIZE      32
#define ASCON_HASH_RATE      8

#define ASCON_HASH_IV                         \
  (((uint64_t)(ASCON_HASH_VARIANT) << 0) |    \
   ((uint64_t)(ASCON_PA_ROUNDS) << 16) |      \
   ((uint64_t)(ASCON_HASH_PB_ROUNDS) << 20) | \
   ((uint64_t)(ASCON_HASH_SIZE * 8) << 24) |  \
   ((uint64_t)(ASCON_HASH_RATE) << 40))

/* ---- ascon.h ------------------------------------------------------------ */
typedef struct {
  uint64_t x[5];
} ascon_state_t;

/* ---- word.h ------------------------------------------------------------- */
/* get byte from 64-bit Ascon word */
#define GETBYTE(x, i) ((uint8_t)((uint64_t)(x) >> (8 * (i))))

/* set byte in 64-bit Ascon word */
#define SETBYTE(b, i) ((uint64_t)(b) << (8 * (i)))

/* set padding byte in 64-bit Ascon word */
#define PAD(i) SETBYTE(0x01, i)

/* load bytes into 64-bit Ascon word */
static inline uint64_t LOADBYTES(const uint8_t* bytes, size_t n) {
  size_t i;
  uint64_t x = 0;
  for (i = 0; i < n; ++i) x |= SETBYTE(bytes[i], i);
  return x;
}

/* store bytes from 64-bit Ascon word */
static inline void STOREBYTES(uint8_t* bytes, uint64_t x, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) bytes[i] = GETBYTE(x, i);
}

/* ---- round.h ------------------------------------------------------------ */
static inline uint64_t ROR(uint64_t x, int n) {
  return x >> n | x << (-n & 63);
}

static inline void ROUND(ascon_state_t* s, uint8_t C) {
  ascon_state_t t;
  /* addition of round constant */
  s->x[2] ^= C;
  /* substitution layer */
  s->x[0] ^= s->x[4];
  s->x[4] ^= s->x[3];
  s->x[2] ^= s->x[1];
  /* start of keccak s-box */
  t.x[0] = s->x[0] ^ (~s->x[1] & s->x[2]);
  t.x[1] = s->x[1] ^ (~s->x[2] & s->x[3]);
  t.x[2] = s->x[2] ^ (~s->x[3] & s->x[4]);
  t.x[3] = s->x[3] ^ (~s->x[4] & s->x[0]);
  t.x[4] = s->x[4] ^ (~s->x[0] & s->x[1]);
  /* end of keccak s-box */
  t.x[1] ^= t.x[0];
  t.x[0] ^= t.x[4];
  t.x[3] ^= t.x[2];
  t.x[2] = ~t.x[2];
  /* linear diffusion layer */
  s->x[0] = t.x[0] ^ ROR(t.x[0], 19) ^ ROR(t.x[0], 28);
  s->x[1] = t.x[1] ^ ROR(t.x[1], 61) ^ ROR(t.x[1], 39);
  s->x[2] = t.x[2] ^ ROR(t.x[2], 1) ^ ROR(t.x[2], 6);
  s->x[3] = t.x[3] ^ ROR(t.x[3], 10) ^ ROR(t.x[3], 17);
  s->x[4] = t.x[4] ^ ROR(t.x[4], 7) ^ ROR(t.x[4], 41);
}

/* ---- permutations.h ----------------------------------------------------- */
static inline void P12(ascon_state_t* s) {
  ROUND(s, 0xf0);
  ROUND(s, 0xe1);
  ROUND(s, 0xd2);
  ROUND(s, 0xc3);
  ROUND(s, 0xb4);
  ROUND(s, 0xa5);
  ROUND(s, 0x96);
  ROUND(s, 0x87);
  ROUND(s, 0x78);
  ROUND(s, 0x69);
  ROUND(s, 0x5a);
  ROUND(s, 0x4b);
}

/* ---- hash.c (reference one-shot) ---------------------------------------- */
void ascon_hash256(const uint8_t *in, size_t len, uint8_t out[ASCON_HASH256_BYTES]) {
  /* initialize */
  ascon_state_t s;
  s.x[0] = ASCON_HASH_IV;
  s.x[1] = 0;
  s.x[2] = 0;
  s.x[3] = 0;
  s.x[4] = 0;
  P12(&s);

  /* absorb full plaintext blocks */
  while (len >= ASCON_HASH_RATE) {
    s.x[0] ^= LOADBYTES(in, 8);
    P12(&s);
    in += ASCON_HASH_RATE;
    len -= ASCON_HASH_RATE;
  }
  /* absorb final plaintext block */
  s.x[0] ^= LOADBYTES(in, len);
  s.x[0] ^= PAD(len);
  P12(&s);

  /* squeeze full output blocks */
  len = ASCON_HASH256_BYTES;
  while (len > ASCON_HASH_RATE) {
    STOREBYTES(out, s.x[0], 8);
    P12(&s);
    out += ASCON_HASH_RATE;
    len -= ASCON_HASH_RATE;
  }
  /* squeeze final output block */
  STOREBYTES(out, s.x[0], len);
}

/* ---- incremental interface (AlgaOps wrapper) ---------------------------- */
void ascon_hash256_init(AsconHash256Ctx *ctx) {
  ascon_state_t s;
  s.x[0] = ASCON_HASH_IV;
  s.x[1] = 0;
  s.x[2] = 0;
  s.x[3] = 0;
  s.x[4] = 0;
  P12(&s);
  memcpy(ctx->x, s.x, sizeof(s.x));
  ctx->buf_len = 0;
}

void ascon_hash256_update(AsconHash256Ctx *ctx, const uint8_t *in, size_t len) {
  ascon_state_t s;
  memcpy(s.x, ctx->x, sizeof(s.x));
  if (ctx->buf_len > 0) {
    size_t take = ASCON_HASH_RATE - ctx->buf_len;
    if (take > len) take = len;
    memcpy(ctx->buf + ctx->buf_len, in, take);
    ctx->buf_len += take;
    in += take;
    len -= take;
    if (ctx->buf_len == ASCON_HASH_RATE) {
      s.x[0] ^= LOADBYTES(ctx->buf, 8);
      P12(&s);
      ctx->buf_len = 0;
    }
  }
  while (len >= ASCON_HASH_RATE) {
    s.x[0] ^= LOADBYTES(in, 8);
    P12(&s);
    in += ASCON_HASH_RATE;
    len -= ASCON_HASH_RATE;
  }
  if (len > 0) {
    memcpy(ctx->buf, in, len);
    ctx->buf_len = len;
  }
  memcpy(ctx->x, s.x, sizeof(s.x));
}

void ascon_hash256_final(AsconHash256Ctx *ctx, uint8_t out[ASCON_HASH256_BYTES]) {
  ascon_state_t s;
  memcpy(s.x, ctx->x, sizeof(s.x));
  /* absorb final (partial) block with padding */
  s.x[0] ^= LOADBYTES(ctx->buf, ctx->buf_len);
  s.x[0] ^= PAD(ctx->buf_len);
  P12(&s);
  /* squeeze */
  size_t len = ASCON_HASH256_BYTES;
  while (len > ASCON_HASH_RATE) {
    STOREBYTES(out, s.x[0], 8);
    P12(&s);
    out += ASCON_HASH_RATE;
    len -= ASCON_HASH_RATE;
  }
  STOREBYTES(out, s.x[0], len);
  memset(ctx, 0, sizeof(*ctx));
}

void ascon_hash256_hex(const uint8_t digest[ASCON_HASH256_BYTES], char out[65]) {
  static const char HEX[] = "0123456789abcdef";
  for (int i = 0; i < ASCON_HASH256_BYTES; i++) {
    out[2 * i]     = HEX[(digest[i] >> 4) & 0xF];
    out[2 * i + 1] = HEX[digest[i] & 0xF];
  }
  out[64] = '\0';
}
