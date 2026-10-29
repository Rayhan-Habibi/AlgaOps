/*
 * ring_buffer.h — bounded FIFO of fixed-size records (windows waiting to be
 * published).
 *
 * The node never blocks sampling on the network: when the buffer is full,
 * push() overwrites the OLDEST record and counts it as dropped.  The gateway
 * then sees the lost sequence numbers as a gap (MISSING_PACKET), never as
 * valid data.  Not thread-safe: the caller serialises access.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  *storage;     /* capacity * item_size bytes, owned by the caller */
    size_t    item_size;
    uint32_t  capacity;
    uint32_t  head;        /* index of the oldest record                      */
    uint32_t  count;
    uint32_t  dropped;     /* records overwritten while full (lifetime)       */
} ring_buffer_t;

/** Returns false for a NULL buffer, zero item size or zero capacity. */
bool ring_buffer_init(ring_buffer_t *rb, void *storage, size_t item_size, uint32_t capacity);

/** Append; returns false when the oldest record had to be dropped to make room. */
bool ring_buffer_push(ring_buffer_t *rb, const void *item);

/** Copy the oldest record without removing it; false when empty. */
bool ring_buffer_peek(const ring_buffer_t *rb, void *out);

/** Remove the oldest record (copied to @p out unless NULL); false when empty. */
bool ring_buffer_pop(ring_buffer_t *rb, void *out);

uint32_t ring_buffer_count(const ring_buffer_t *rb);
bool     ring_buffer_full(const ring_buffer_t *rb);
void     ring_buffer_clear(ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
