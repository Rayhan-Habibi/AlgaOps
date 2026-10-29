/*
 * audit_chain.c — Ascon-Hash256 chain links and a minimal reader for the
 * exported event-line format (see audit_chain.h).
 */

#include "../include/audit_chain.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void audit_chain_link(const uint8_t prev[AUDIT_HASH_BYTES], uint64_t seq,
                      const uint8_t *payload, size_t len,
                      uint8_t out[AUDIT_HASH_BYTES])
{
    static const uint8_t DOMAIN[] = AUDIT_DOMAIN; /* includes trailing NUL */
    uint8_t be[8];
    for (int i = 0; i < 8; i++) be[i] = (uint8_t)(seq >> (56 - 8 * i));

    AsconHash256Ctx ctx;
    ascon_hash256_init(&ctx);
    ascon_hash256_update(&ctx, DOMAIN, sizeof(DOMAIN));   /* "…v1" || 0x00 */
    ascon_hash256_update(&ctx, prev, AUDIT_HASH_BYTES);
    ascon_hash256_update(&ctx, be, sizeof(be));
    if (payload && len) ascon_hash256_update(&ctx, payload, len);
    ascon_hash256_final(&ctx, out);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int audit_hex_decode(const char *hex, uint8_t out[AUDIT_HASH_BYTES])
{
    if (!hex || strlen(hex) != 2u * AUDIT_HASH_BYTES) return -1;
    for (int i = 0; i < AUDIT_HASH_BYTES; i++) {
        int hi = hexval((unsigned char)hex[2 * i]);
        int lo = hexval((unsigned char)hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void audit_chain_init(AuditChain *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
}

void audit_chain_append(AuditChain *c, const uint8_t *payload, size_t len,
                        uint8_t out_hash[AUDIT_HASH_BYTES])
{
    uint8_t h[AUDIT_HASH_BYTES];
    c->seq++;
    audit_chain_link(c->head, c->seq, payload, len, h);
    memcpy(c->head, h, AUDIT_HASH_BYTES);
    if (out_hash) memcpy(out_hash, h, AUDIT_HASH_BYTES);
}

AuditVerifyCode audit_chain_verify_step(AuditChain *c, uint64_t seq,
                                        const char *prev_hex, const char *hash_hex,
                                        const uint8_t *payload, size_t len)
{
    uint8_t prev[AUDIT_HASH_BYTES], claimed[AUDIT_HASH_BYTES], h[AUDIT_HASH_BYTES];
    if (audit_hex_decode(prev_hex, prev) != 0 || audit_hex_decode(hash_hex, claimed) != 0) {
        return AUDIT_VERIFY_BAD_ENCODING;
    }
    if (seq != c->seq + 1u) return AUDIT_VERIFY_BAD_SEQ;
    if (memcmp(prev, c->head, AUDIT_HASH_BYTES) != 0) return AUDIT_VERIFY_BROKEN_LINK;
    audit_chain_link(prev, seq, payload, len, h);
    if (memcmp(h, claimed, AUDIT_HASH_BYTES) != 0) return AUDIT_VERIFY_HASH_MISMATCH;
    memcpy(c->head, h, AUDIT_HASH_BYTES);
    c->seq = seq;
    return AUDIT_VERIFY_OK;
}

const char *audit_verify_code_str(AuditVerifyCode code)
{
    switch (code) {
        case AUDIT_VERIFY_OK:            return "OK";
        case AUDIT_VERIFY_BAD_SEQ:       return "BAD_SEQUENCE";
        case AUDIT_VERIFY_BROKEN_LINK:   return "BROKEN_LINK";
        case AUDIT_VERIFY_HASH_MISMATCH: return "HASH_MISMATCH";
        case AUDIT_VERIFY_BAD_ENCODING:  return "BAD_ENCODING";
        default:                         return "UNKNOWN";
    }
}

/* ---- minimal JSON field access (our own one-line format) --------------- */

/* Find `"key":` at nesting depth 1 outside of strings; returns pointer to
 * the first character of the value, or NULL. */
static const char *find_value(const char *line, const char *key)
{
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    for (const char *p = line; *p; p++) {
        char ch = *p;
        if (in_str) {
            if (ch == '\\' && p[1]) { p++; continue; }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '{' || ch == '[') { depth++; continue; }
        if (ch == '}' || ch == ']') { depth--; continue; }
        if (ch != '"') continue;
        /* a string starts here: is it our key at depth 1? */
        if (depth == 1 && strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                return q;
            }
        }
        in_str = true;
    }
    return NULL;
}

static size_t put_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int read_hex4(const char *p, uint32_t *v)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval((unsigned char)p[i]);
        if (h < 0) return -1;
        r = (r << 4) | (uint32_t)h;
    }
    *v = r;
    return 0;
}

long audit_json_get_string(const char *line, const char *key, char *out, size_t out_len)
{
    if (!line || !key || !out || out_len == 0) return -1;
    const char *p = find_value(line, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char tmp[4];
        size_t w = 0;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  tmp[0] = '"';  w = 1; break;
                case '\\': tmp[0] = '\\'; w = 1; break;
                case '/':  tmp[0] = '/';  w = 1; break;
                case 'b':  tmp[0] = '\b'; w = 1; break;
                case 'f':  tmp[0] = '\f'; w = 1; break;
                case 'n':  tmp[0] = '\n'; w = 1; break;
                case 'r':  tmp[0] = '\r'; w = 1; break;
                case 't':  tmp[0] = '\t'; w = 1; break;
                case 'u': {
                    uint32_t cp;
                    if (read_hex4(p + 1, &cp) != 0) return -1;
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (p[1] != '\\' || p[2] != 'u' || read_hex4(p + 3, &lo) != 0 ||
                            lo < 0xDC00 || lo > 0xDFFF) return -1;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        p += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return -1;
                    }
                    w = put_utf8(cp, tmp);
                    break;
                }
                default: return -1;
            }
            p++;
        } else {
            if ((unsigned char)*p < 0x20) return -1;
            tmp[0] = *p++;
            w = 1;
        }
        if (n + w >= out_len) return -1;
        memcpy(out + n, tmp, w);
        n += w;
    }
    if (*p != '"') return -1;
    out[n] = '\0';
    return (long)n;
}

int audit_json_get_u64(const char *line, const char *key, uint64_t *out)
{
    if (!line || !key || !out) return -1;
    const char *p = find_value(line, key);
    if (!p || !isdigit((unsigned char)*p)) return -1;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return -1;
    *out = (uint64_t)v;
    return 0;
}

long audit_json_escape(const char *in, size_t in_len, char *out, size_t out_len)
{
    static const char HEX[] = "0123456789abcdef";
    if (!in || !out || out_len < 3) return -1;
    size_t n = 0;
    out[n++] = '"';
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        char esc[7];
        size_t w;
        switch (c) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  w = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; w = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  w = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  w = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  w = 2; break;
            default:
                if (c < 0x20) {
                    esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                    esc[4] = HEX[c >> 4]; esc[5] = HEX[c & 0xF];
                    w = 6;
                } else {
                    esc[0] = (char)c;
                    w = 1;
                }
        }
        if (n + w + 2 > out_len) return -1;
        memcpy(out + n, esc, w);
        n += w;
    }
    out[n++] = '"';
    out[n] = '\0';
    return (long)n;
}
