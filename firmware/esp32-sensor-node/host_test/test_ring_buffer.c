/* Bounded window buffer: FIFO order, overwrite-oldest with a drop count. */

#include <stdint.h>

#include "ring_buffer.h"
#include "test_harness.h"

typedef struct {
    uint32_t seq;
    float    value;
} item_t;

static int test_init_rejects_invalid_arguments(void)
{
    ring_buffer_t rb;
    item_t store[4];
    ASSERT(!ring_buffer_init(NULL, store, sizeof(item_t), 4), "NULL buffer");
    ASSERT(!ring_buffer_init(&rb, NULL, sizeof(item_t), 4), "NULL storage");
    ASSERT(!ring_buffer_init(&rb, store, 0, 4), "zero item size");
    ASSERT(!ring_buffer_init(&rb, store, sizeof(item_t), 0), "zero capacity");
    ASSERT(ring_buffer_init(&rb, store, sizeof(item_t), 4), "valid");
    item_t out;
    ASSERT(!ring_buffer_peek(&rb, &out) && !ring_buffer_pop(&rb, &out), "empty");
    return 0;
}

static int test_fifo_order(void)
{
    ring_buffer_t rb;
    item_t store[4];
    ring_buffer_init(&rb, store, sizeof(item_t), 4);
    for (uint32_t i = 1; i <= 3; i++) {
        item_t it = { i, (float)i * 0.5f };
        ASSERT(ring_buffer_push(&rb, &it), "room left");
    }
    item_t out;
    ASSERT(ring_buffer_peek(&rb, &out) && out.seq == 1, "peek oldest");
    ASSERT(ring_buffer_count(&rb) == 3, "peek keeps the record");
    for (uint32_t i = 1; i <= 3; i++) {
        ASSERT(ring_buffer_pop(&rb, &out) && out.seq == i, "pop in order");
    }
    ASSERT(ring_buffer_count(&rb) == 0 && rb.dropped == 0, "empty, nothing dropped");
    return 0;
}

static int test_overflow_drops_oldest(void)
{
    ring_buffer_t rb;
    item_t store[3];
    ring_buffer_init(&rb, store, sizeof(item_t), 3);
    for (uint32_t i = 1; i <= 3; i++) {
        item_t it = { i, 0.0f };
        ring_buffer_push(&rb, &it);
    }
    ASSERT(ring_buffer_full(&rb), "full");
    item_t it = { 4, 0.0f };
    ASSERT(!ring_buffer_push(&rb, &it), "push reports the drop");
    ASSERT(rb.dropped == 1 && ring_buffer_count(&rb) == 3, "one dropped, still full");
    item_t out;
    ASSERT(ring_buffer_pop(&rb, &out) && out.seq == 2, "oldest (1) was dropped");
    ASSERT(ring_buffer_pop(&rb, NULL), "pop without copy");
    ASSERT(ring_buffer_pop(&rb, &out) && out.seq == 4, "newest kept");
    return 0;
}

static int test_long_run_wraps_consistently(void)
{
    ring_buffer_t rb;
    item_t store[5];
    ring_buffer_init(&rb, store, sizeof(item_t), 5);
    uint32_t next_expected = 1;
    uint32_t pushed = 0;
    for (uint32_t i = 1; i <= 1000; i++) {
        item_t it = { i, 0.0f };
        ring_buffer_push(&rb, &it);
        pushed++;
        if (i % 3 == 0) {                 /* consumer slower than the producer */
            item_t out;
            ASSERT(ring_buffer_pop(&rb, &out), "non-empty");
            ASSERT(out.seq >= next_expected, "never goes backwards");
            next_expected = out.seq + 1;
        }
    }
    item_t out;
    uint32_t last = 0;
    while (ring_buffer_pop(&rb, &out)) {
        ASSERT(out.seq > last, "strictly increasing");
        last = out.seq;
    }
    ASSERT(last == 1000, "newest record survives");
    ASSERT(rb.dropped > 0, "the slow consumer lost records");
    ring_buffer_clear(&rb);
    ASSERT(ring_buffer_count(&rb) == 0, "cleared");
    (void)pushed;
    return 0;
}

int main(void)
{
    printf("ring_buffer\n");
    RUN_TEST(test_init_rejects_invalid_arguments);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_long_run_wraps_consistently);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
