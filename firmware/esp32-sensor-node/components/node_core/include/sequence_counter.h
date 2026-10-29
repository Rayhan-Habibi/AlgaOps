/*
 * sequence_counter.h — per-boot window sequence numbers.
 *
 * Every boot gets a new random boot_id and the sequence restarts at 1; it
 * wraps after 2^32 - 1 to 0.  The gateway orders numbers with RFC 1982 serial
 * arithmetic, mirrored here by sequence_distance() for the host tests.
 */

#ifndef SEQUENCE_COUNTER_H
#define SEQUENCE_COUNTER_H

#include <stdint.h>

typedef struct {
    uint32_t next;     /* number the next window receives */
    uint64_t issued;   /* windows numbered since boot     */
} sequence_counter_t;

void     sequence_counter_init(sequence_counter_t *c);
uint32_t sequence_counter_next(sequence_counter_t *c);

/** Signed RFC 1982 distance from @p from to @p to (positive: @p to is newer). */
int64_t  sequence_distance(uint32_t from, uint32_t to);

#endif /* SEQUENCE_COUNTER_H */
