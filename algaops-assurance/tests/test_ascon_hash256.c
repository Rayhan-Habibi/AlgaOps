/*
 * test_ascon_hash256.c — Ascon-Hash256 against the official NIST SP 800-232
 * KAT vectors (tests/vectors/ascon_hash256_kat.txt, copied verbatim from the
 * ascon-c reference repository), plus equivalence of the incremental API.
 */

#include "test_harness.h"
#include "../include/ascon_hash256.h"

#include <stdint.h>
#include <stdlib.h>

#define MAX_MSG 1100

typedef struct {
    int     count;
    size_t  len;
    uint8_t msg[MAX_MSG];
    uint8_t md[32];
} Kat;

static Kat g_kats[200];
static int g_nkats = 0;

static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, size_t max, size_t *len)
{
    size_t n = 0;
    while (s[0] && s[0] != '\n' && s[0] != '\r') {
        int hi = hexnib((unsigned char)s[0]), lo = hexnib((unsigned char)s[1]);
        if (hi < 0 || lo < 0 || n >= max) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    *len = n;
    return 0;
}

static int load_kats(void)
{
    char path[512];
    FILE *f = fopen(src_path(path, sizeof(path), "tests/vectors/ascon_hash256_kat.txt"), "r");
    if (!f) return -1;
    static char line[4096];
    Kat *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (strncmp(line, "Count = ", 8) == 0) {
            if (g_nkats >= (int)(sizeof(g_kats) / sizeof(g_kats[0]))) break;
            cur = &g_kats[g_nkats++];
            memset(cur, 0, sizeof(*cur));
            cur->count = atoi(line + 8);
        } else if (cur && strncmp(line, "Msg = ", 6) == 0) {
            if (parse_hex(line + 6, cur->msg, MAX_MSG, &cur->len) != 0) { fclose(f); return -1; }
        } else if (cur && strncmp(line, "MD = ", 5) == 0) {
            size_t n = 0;
            if (parse_hex(line + 5, cur->md, 32, &n) != 0 || n != 32) { fclose(f); return -1; }
        }
    }
    fclose(f);
    return g_nkats;
}

static int test_kat_file_loaded(void)
{
    ASSERT(load_kats() >= 131, "expected at least 131 KAT entries");
    ASSERT(g_kats[0].count == 1 && g_kats[0].len == 0, "first KAT must be the empty message");
    ASSERT(g_kats[g_nkats - 1].len == 1024, "last KAT must be the 1024-byte message");
    return 0;
}

static int test_kat_one_shot(void)
{
    for (int i = 0; i < g_nkats; i++) {
        uint8_t h[32];
        ascon_hash256(g_kats[i].msg, g_kats[i].len, h);
        if (memcmp(h, g_kats[i].md, 32) != 0) {
            fprintf(stderr, "  KAT Count=%d mismatch\n", g_kats[i].count);
            return 1;
        }
    }
    return 0;
}

static int test_kat_incremental_all_splits(void)
{
    /* Every split point of the first 130 messages, two chunks. */
    for (int i = 0; i < g_nkats && g_kats[i].len < 130; i++) {
        for (size_t cut = 0; cut <= g_kats[i].len; cut++) {
            AsconHash256Ctx c;
            uint8_t h[32];
            ascon_hash256_init(&c);
            ascon_hash256_update(&c, g_kats[i].msg, cut);
            ascon_hash256_update(&c, g_kats[i].msg + cut, g_kats[i].len - cut);
            ascon_hash256_final(&c, h);
            if (memcmp(h, g_kats[i].md, 32) != 0) {
                fprintf(stderr, "  Count=%d cut=%u mismatch\n", g_kats[i].count, (unsigned)cut);
                return 1;
            }
        }
    }
    return 0;
}

static int test_kat_incremental_byte_by_byte(void)
{
    const Kat *k = &g_kats[g_nkats - 1];   /* 1024 bytes */
    AsconHash256Ctx c;
    uint8_t h[32];
    ascon_hash256_init(&c);
    for (size_t i = 0; i < k->len; i++) ascon_hash256_update(&c, k->msg + i, 1);
    ascon_hash256_final(&c, h);
    ASSERT(memcmp(h, k->md, 32) == 0, "byte-by-byte hashing must match KAT");

    /* irregular chunk sizes */
    size_t pos = 0, step = 1;
    ascon_hash256_init(&c);
    while (pos < k->len) {
        size_t take = step > k->len - pos ? k->len - pos : step;
        ascon_hash256_update(&c, k->msg + pos, take);
        pos += take;
        step = (step * 7 + 3) % 37 + 1;
    }
    ascon_hash256_final(&c, h);
    ASSERT(memcmp(h, k->md, 32) == 0, "irregular chunking must match KAT");
    return 0;
}

static int test_hex_encoding(void)
{
    char hex[65];
    ascon_hash256_hex(g_kats[0].md, hex);
    ASSERT(strcmp(hex, "0b3be5850f2f6b98caf29f8fdea89b64a1fa70aa249b8f839bd53baa304d92b2") == 0,
           "hex encoding of empty-message digest");
    ASSERT(strlen(hex) == 64, "hex length");
    return 0;
}

static int test_single_bit_avalanche(void)
{
    uint8_t m[64], h1[32], h2[32];
    for (int i = 0; i < 64; i++) m[i] = (uint8_t)i;
    ascon_hash256(m, sizeof(m), h1);
    m[17] ^= 0x01;
    ascon_hash256(m, sizeof(m), h2);
    int diff_bits = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t x = (uint8_t)(h1[i] ^ h2[i]);
        while (x) { diff_bits += x & 1; x >>= 1; }
    }
    ASSERT(diff_bits > 64 && diff_bits < 192, "one-bit change must flip roughly half the digest");
    return 0;
}

int main(void)
{
    printf("=== Ascon-Hash256 (NIST SP 800-232) Tests ===\n");
    RUN_TEST(test_kat_file_loaded);
    RUN_TEST(test_kat_one_shot);
    RUN_TEST(test_kat_incremental_all_splits);
    RUN_TEST(test_kat_incremental_byte_by_byte);
    RUN_TEST(test_hex_encoding);
    RUN_TEST(test_single_bit_avalanche);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
