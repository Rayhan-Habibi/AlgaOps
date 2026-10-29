/* ring_buffer.c — see ring_buffer.h */

#include "ring_buffer.h"

#include <string.h>

bool ring_buffer_init(ring_buffer_t *rb, void *storage, size_t item_size, uint32_t capacity)
{
    if (!rb || !storage || item_size == 0 || capacity == 0) return false;
    rb->storage   = (uint8_t *)storage;
    rb->item_size = item_size;
    rb->capacity  = capacity;
    rb->head      = 0;
    rb->count     = 0;
    rb->dropped   = 0;
    return true;
}

static uint8_t *slot(const ring_buffer_t *rb, uint32_t index)
{
    return rb->storage + (size_t)(index % rb->capacity) * rb->item_size;
}

bool ring_buffer_push(ring_buffer_t *rb, const void *item)
{
    if (!rb || !item || rb->capacity == 0) return false;
    bool kept_all = true;
    if (rb->count == rb->capacity) {
        rb->head = (rb->head + 1) % rb->capacity;   /* drop the oldest record */
        rb->count--;
        rb->dropped++;
        kept_all = false;
    }
    memcpy(slot(rb, rb->head + rb->count), item, rb->item_size);
    rb->count++;
    return kept_all;
}

bool ring_buffer_peek(const ring_buffer_t *rb, void *out)
{
    if (!rb || !out || rb->count == 0) return false;
    memcpy(out, slot(rb, rb->head), rb->item_size);
    return true;
}

bool ring_buffer_pop(ring_buffer_t *rb, void *out)
{
    if (!rb || rb->count == 0) return false;
    if (out) memcpy(out, slot(rb, rb->head), rb->item_size);
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count--;
    return true;
}

uint32_t ring_buffer_count(const ring_buffer_t *rb) { return rb ? rb->count : 0; }

bool ring_buffer_full(const ring_buffer_t *rb) { return rb && rb->count == rb->capacity; }

void ring_buffer_clear(ring_buffer_t *rb)
{
    if (!rb) return;
    rb->head  = 0;
    rb->count = 0;
}
