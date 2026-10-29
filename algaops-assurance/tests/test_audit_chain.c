/*
 * test_audit_chain.c — hash-chain link function, tamper detection and the
 * minimal JSON field reader used by --verify-evidence.
 */

#include "test_harness.h"
#include "../include/audit_chain.h"

#include <stdint.h>

#define N_EVENTS 6

typedef struct {
    uint64_t seq;
    char     payload[128];
    char     prev[65];
    char     hash[65];
} Ev;

static void build_chain(Ev ev[N_EVENTS])
{
    AuditChain c;
    audit_chain_init(&c);
    for (int i = 0; i < N_EVENTS; i++) {
        uint8_t h[32];
        snprintf(ev[i].payload, sizeof(ev[i].payload),
                 "{\"type\":\"window.diagnosed\",\"seq\":%d,\"outcome\":\"NORMAL\"}", i + 1);
        ascon_hash256_hex(c.head, ev[i].prev);
        audit_chain_append(&c, (const uint8_t *)ev[i].payload, strlen(ev[i].payload), h);
        ascon_hash256_hex(h, ev[i].hash);
        ev[i].seq = c.seq;
    }
}

static AuditVerifyCode verify(Ev *ev, int n, int *bad_index)
{
    AuditChain c;
    audit_chain_init(&c);
    for (int i = 0; i < n; i++) {
        AuditVerifyCode code = audit_chain_verify_step(&c, ev[i].seq, ev[i].prev, ev[i].hash,
                                                       (const uint8_t *)ev[i].payload,
                                                       strlen(ev[i].payload));
        if (code != AUDIT_VERIFY_OK) { *bad_index = i; return code; }
    }
    *bad_index = -1;
    return AUDIT_VERIFY_OK;
}

static int test_link_deterministic_and_bound(void)
{
    uint8_t zero[32] = { 0 }, a[32], b[32], c[32], d[32];
    const uint8_t *p = (const uint8_t *)"{}";
    audit_chain_link(zero, 1, p, 2, a);
    audit_chain_link(zero, 1, p, 2, b);
    ASSERT(memcmp(a, b, 32) == 0, "link must be deterministic");
    audit_chain_link(zero, 2, p, 2, c);
    ASSERT(memcmp(a, c, 32) != 0, "sequence number must be bound into the hash");
    uint8_t other[32] = { 1 };
    audit_chain_link(other, 1, p, 2, d);
    ASSERT(memcmp(a, d, 32) != 0, "previous hash must be bound into the hash");
    return 0;
}

static int test_link_equals_manual_concatenation(void)
{
    /* hash = Ascon-Hash256("AlgaOps/audit/v1" 0x00 || prev || be64(seq) || payload) */
    uint8_t prev[32];
    for (int i = 0; i < 32; i++) prev[i] = (uint8_t)(0xA0 + i);
    const char *payload = "{\"k\":\"v\"}";
    uint8_t msg[256];
    size_t n = 0;
    memcpy(msg, "AlgaOps/audit/v1", 16); n = 16;
    msg[n++] = 0x00;
    memcpy(msg + n, prev, 32); n += 32;
    uint64_t seq = 0x0102030405060708ull;
    for (int i = 0; i < 8; i++) msg[n++] = (uint8_t)(seq >> (56 - 8 * i));
    memcpy(msg + n, payload, strlen(payload)); n += strlen(payload);
    uint8_t want[32], got[32];
    ascon_hash256(msg, n, want);
    audit_chain_link(prev, seq, (const uint8_t *)payload, strlen(payload), got);
    ASSERT(memcmp(want, got, 32) == 0, "link must equal the documented concatenation");
    return 0;
}

static int test_intact_chain_verifies(void)
{
    Ev ev[N_EVENTS];
    build_chain(ev);
    int bad;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_OK, "intact chain must verify");
    ASSERT(strcmp(ev[0].prev, "0000000000000000000000000000000000000000000000000000000000000000") == 0,
           "genesis prev hash is 32 zero bytes");
    return 0;
}

static int test_modification_detected(void)
{
    Ev ev[N_EVENTS];
    build_chain(ev);
    ev[3].payload[strlen(ev[3].payload) - 3] = 'X';  /* NORMAL -> NORMXL */
    int bad;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_HASH_MISMATCH, "payload edit must be detected");
    ASSERT(bad == 3, "first failing event must be the modified one");
    return 0;
}

static int test_modification_with_rehash_breaks_link(void)
{
    /* Attacker edits event 2 and recomputes its hash, but not the successors. */
    Ev ev[N_EVENTS];
    build_chain(ev);
    strcpy(ev[2].payload, "{\"type\":\"window.diagnosed\",\"seq\":3,\"outcome\":\"CHECK_SENSOR\"}");
    uint8_t prev[32], h[32];
    audit_hex_decode(ev[2].prev, prev);
    audit_chain_link(prev, 3, (const uint8_t *)ev[2].payload, strlen(ev[2].payload), h);
    ascon_hash256_hex(h, ev[2].hash);
    int bad;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_BROKEN_LINK, "successor link must break");
    ASSERT(bad == 3, "break reported at the successor");
    return 0;
}

static int test_deletion_detected(void)
{
    Ev ev[N_EVENTS];
    build_chain(ev);
    Ev cut[N_EVENTS - 1];
    for (int i = 0, j = 0; i < N_EVENTS; i++) if (i != 2) cut[j++] = ev[i];
    int bad;
    ASSERT(verify(cut, N_EVENTS - 1, &bad) == AUDIT_VERIFY_BAD_SEQ, "deletion leaves a seq gap");
    ASSERT(bad == 2, "gap reported where the event is missing");
    return 0;
}

static int test_reordering_detected(void)
{
    Ev ev[N_EVENTS];
    build_chain(ev);
    Ev tmp = ev[1]; ev[1] = ev[2]; ev[2] = tmp;
    int bad;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_BAD_SEQ, "reordering must be detected");
    /* Renumbering the swapped events does not help: links no longer match. */
    ev[1].seq = 2; ev[2].seq = 3;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_BROKEN_LINK, "renumbered swap breaks links");
    return 0;
}

static int test_bad_encoding_detected(void)
{
    Ev ev[N_EVENTS];
    build_chain(ev);
    ev[0].hash[5] = 'Z';
    int bad;
    ASSERT(verify(ev, N_EVENTS, &bad) == AUDIT_VERIFY_BAD_ENCODING, "non-hex digest rejected");
    return 0;
}

static int test_json_reader_roundtrip(void)
{
    const char *payload = "{\"reason\":\"vib 0.42 g \\\"quoted\\\" \xC2\xB0""C\",\"n\":1}";
    char esc[512];
    ASSERT(audit_json_escape(payload, strlen(payload), esc, sizeof(esc)) > 0, "escape");
    char line[1024];
    snprintf(line, sizeof(line),
             "{\"seq\":42,\"type\":\"x\",\"payload\":%s,\"prev_hash\":\"%064d\",\"hash\":\"ab\"}",
             esc, 0);
    char out[512];
    long n = audit_json_get_string(line, "payload", out, sizeof(out));
    ASSERT(n == (long)strlen(payload), "decoded payload length");
    ASSERT(strcmp(out, payload) == 0, "payload round trip must be byte exact");
    uint64_t seq = 0;
    ASSERT(audit_json_get_u64(line, "seq", &seq) == 0 && seq == 42, "seq field");
    char h[80];
    ASSERT(audit_json_get_string(line, "hash", h, sizeof(h)) == 2, "hash (not prev_hash) field");
    ASSERT(audit_json_get_string(line, "missing", h, sizeof(h)) == -1, "missing key");
    return 0;
}

static int test_json_reader_unicode_escapes(void)
{
    const char *line = "{\"payload\":\"\\u00b0C \\ud83d\\ude00 \\n\"}";
    char out[64];
    long n = audit_json_get_string(line, "payload", out, sizeof(out));
    ASSERT(n == 10, "UTF-8 length of decoded escapes");
    ASSERT(memcmp(out, "\xC2\xB0" "C \xF0\x9F\x98\x80 \n", 10) == 0, "escape decoding");
    ASSERT(audit_json_get_string("{\"payload\":\"\\ud83d\"}", "payload", out, sizeof(out)) == -1,
           "lone surrogate rejected");
    return 0;
}

static int test_json_reader_ignores_nested_keys(void)
{
    const char *line = "{\"payload\":\"{\\\"hash\\\":\\\"inner\\\"}\",\"hash\":\"outer\"}";
    char out[64];
    ASSERT(audit_json_get_string(line, "hash", out, sizeof(out)) == 5, "outer key length");
    ASSERT(strcmp(out, "outer") == 0, "key inside the escaped payload must not match");
    return 0;
}

int main(void)
{
    printf("=== Audit Chain Tests ===\n");
    RUN_TEST(test_link_deterministic_and_bound);
    RUN_TEST(test_link_equals_manual_concatenation);
    RUN_TEST(test_intact_chain_verifies);
    RUN_TEST(test_modification_detected);
    RUN_TEST(test_modification_with_rehash_breaks_link);
    RUN_TEST(test_deletion_detected);
    RUN_TEST(test_reordering_detected);
    RUN_TEST(test_bad_encoding_detected);
    RUN_TEST(test_json_reader_roundtrip);
    RUN_TEST(test_json_reader_unicode_escapes);
    RUN_TEST(test_json_reader_ignores_nested_keys);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
