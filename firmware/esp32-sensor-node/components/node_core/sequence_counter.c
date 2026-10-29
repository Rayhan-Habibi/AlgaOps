/* sequence_counter.c — see sequence_counter.h */

#include "sequence_counter.h"

#include <stddef.h>

void sequence_counter_init(sequence_counter_t *c)
{
    if (!c) return;
    c->next   = 1;
    c->issued = 0;
}

uint32_t sequence_counter_next(sequence_counter_t *c)
{
    if (!c) return 0;
    uint32_t v = c->next;
    c->next = v + 1u;          /* unsigned wrap: 0xFFFFFFFF -> 0 */
    c->issued++;
    return v;
}

int64_t sequence_distance(uint32_t from, uint32_t to)
{
    uint32_t d = to - from;    /* modulo 2^32 */
    if (d == 0x80000000u) return 0x80000000LL;   /* undefined in RFC 1982: report as far */
    return d < 0x80000000u ? (int64_t)d : (int64_t)d - 0x100000000LL;
}
