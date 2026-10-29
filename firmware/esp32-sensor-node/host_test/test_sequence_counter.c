/* Per-boot sequence numbers and the RFC 1982 distance used by the gateway. */

#include "sequence_counter.h"
#include "test_harness.h"

static int test_starts_at_one_and_increments(void)
{
    sequence_counter_t c;
    sequence_counter_init(&c);
    ASSERT(sequence_counter_next(&c) == 1, "first window is seq 1");
    ASSERT(sequence_counter_next(&c) == 2, "then 2");
    ASSERT(c.issued == 2, "issued count");
    return 0;
}

static int test_wraps_after_uint32_max(void)
{
    sequence_counter_t c;
    sequence_counter_init(&c);
    c.next = 0xFFFFFFFEu;
    ASSERT(sequence_counter_next(&c) == 0xFFFFFFFEu, "near the end");
    ASSERT(sequence_counter_next(&c) == 0xFFFFFFFFu, "last value");
    ASSERT(sequence_counter_next(&c) == 0u, "wraps to 0");
    ASSERT(sequence_counter_next(&c) == 1u, "continues");
    return 0;
}

static int test_rfc1982_distance(void)
{
    ASSERT(sequence_distance(1, 2) == 1, "forward");
    ASSERT(sequence_distance(2, 1) == -1, "backward");
    ASSERT(sequence_distance(0xFFFFFFFFu, 0) == 1, "forward across the wrap");
    ASSERT(sequence_distance(0, 0xFFFFFFFFu) == -1, "backward across the wrap");
    ASSERT(sequence_distance(10, 10) == 0, "same");
    ASSERT(sequence_distance(0, 0x7FFFFFFFu) == 0x7FFFFFFF, "largest forward step");
    ASSERT(sequence_distance(0, 0x80000000u) == 0x80000000LL, "half range: undefined, far");
    return 0;
}

int main(void)
{
    printf("sequence_counter\n");
    RUN_TEST(test_starts_at_one_and_increments);
    RUN_TEST(test_wraps_after_uint32_max);
    RUN_TEST(test_rfc1982_distance);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
