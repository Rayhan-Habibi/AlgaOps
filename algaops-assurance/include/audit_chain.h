/**
 * @file audit_chain.h
 * @brief Tamper-evident hash chain link function (Ascon-Hash256).
 *
 *   hash_i = Ascon-Hash256( "AlgaOps/audit/v1" || 0x00 || hash_{i-1} ||
 *                           be64(i) || payload_i )
 *
 * hash_0 is 32 zero bytes and sequence numbers start at 1.  Binding the
 * sequence number and the previous hash makes modification, deletion,
 * insertion and reordering of events detectable.  Truncation of the newest
 * events is only detectable against an externally retained checkpoint.
 * An attacker who can rewrite the whole log AND every retained checkpoint
 * can forge a consistent chain; the chain is tamper-evident, not tamper-proof.
 *
 * Exported event line format (one JSON object per line, used by the C CLI
 * evidence export and by the gateway export alike):
 *   {"seq":N,"ts_ms":T,"type":"...","node_id":"...","payload":"<JSON text>",
 *    "prev_hash":"<64 hex>","hash":"<64 hex>"}
 * The payload is the exact UTF-8 text that was hashed, carried as a JSON
 * string so that it survives re-serialisation of the outer object.
 */

#ifndef AUDIT_CHAIN_H
#define AUDIT_CHAIN_H

#include "algaops_types.h"
#include "algaops_export.h"
#include "ascon_hash256.h"

#include <stddef.h>
#include <stdint.h>

#define AUDIT_HASH_BYTES ASCON_HASH256_BYTES
#define AUDIT_DOMAIN     "AlgaOps/audit/v1"

/** Compute one chain link. */
ALGAOPS_API void audit_chain_link(const uint8_t prev[AUDIT_HASH_BYTES], uint64_t seq,
                                  const uint8_t *payload, size_t len,
                                  uint8_t out[AUDIT_HASH_BYTES]);

/** Decode 64 hex characters; returns 0 on success, -1 otherwise. */
ALGAOPS_API int audit_hex_decode(const char *hex, uint8_t out[AUDIT_HASH_BYTES]);

typedef struct {
    uint8_t  head[AUDIT_HASH_BYTES];
    uint64_t seq;            /**< sequence number of the head (0 = empty) */
} AuditChain;

void audit_chain_init(AuditChain *c);

/** Append a payload: increments seq, writes the new head to @p out_hash. */
void audit_chain_append(AuditChain *c, const uint8_t *payload, size_t len,
                        uint8_t out_hash[AUDIT_HASH_BYTES]);

typedef enum {
    AUDIT_VERIFY_OK = 0,
    AUDIT_VERIFY_BAD_SEQ,        /**< gap, duplicate or reordering of seq   */
    AUDIT_VERIFY_BROKEN_LINK,    /**< prev_hash != hash of previous event   */
    AUDIT_VERIFY_HASH_MISMATCH,  /**< recomputed hash differs (modified)    */
    AUDIT_VERIFY_BAD_ENCODING    /**< malformed line / hex                  */
} AuditVerifyCode;

/** Verify the next event against the running chain state and advance it. */
AuditVerifyCode audit_chain_verify_step(AuditChain *c, uint64_t seq,
                                        const char *prev_hex, const char *hash_hex,
                                        const uint8_t *payload, size_t len);

const char *audit_verify_code_str(AuditVerifyCode code);

/**
 * @brief Extract a top-level string field of a one-line JSON object and
 *        decode its JSON escapes into UTF-8.
 * @return decoded length, or -1 if the field is missing/malformed/too long.
 */
long audit_json_get_string(const char *line, const char *key, char *out, size_t out_len);

/** Extract a top-level unsigned integer field; returns 0 on success. */
int audit_json_get_u64(const char *line, const char *key, uint64_t *out);

/**
 * @brief Write @p in as a JSON string literal (with quotes) into @p out.
 * @return number of bytes written (excluding NUL), or -1 if out is too small.
 */
long audit_json_escape(const char *in, size_t in_len, char *out, size_t out_len);

#endif /* AUDIT_CHAIN_H */
