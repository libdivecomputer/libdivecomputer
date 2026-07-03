#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

// Include the actual production header
#include "src/citizen_aqualand_parser.h"

START_TEST(test_allocation_size_overflow_protection)
{
    // Invariant: Multiplication for allocation size must not overflow
    // or must be properly validated before allocation
    
    // Test cases: boundary values that could cause overflow
    size_t test_cases[][2] = {
        // {count, size} pairs
        {SIZE_MAX, 2},           // Exact exploit case - multiplication wraps
        {SIZE_MAX / 2 + 1, 2},   // Boundary case - just overflows
        {100, 10},               // Valid case - no overflow
        {0, SIZE_MAX},           // Zero count - should be safe
        {SIZE_MAX, 1}            // Max count with size 1 - should not overflow
    };
    
    int num_cases = sizeof(test_cases) / sizeof(test_cases[0]);
    
    for (int i = 0; i < num_cases; i++) {
        size_t count = test_cases[i][0];
        size_t size = test_cases[i][1];
        
        // Call the actual production function that performs the allocation
        // The function should either:
        // 1. Properly check for overflow before allocating
        // 2. Use safe multiplication that prevents overflow
        // 3. Return appropriate error on overflow
        
        void *result = allocate_buffer_safely(count, size);
        
        // Security property: If allocation succeeds, it must be the correct size
        // OR if overflow would occur, allocation must fail (return NULL)
        if (count > 0 && size > 0) {
            if (count > SIZE_MAX / size) {
                // This multiplication would overflow - allocation MUST fail
                ck_assert_msg(result == NULL, 
                    "Allocation should fail for overflow case: count=%zu, size=%zu", 
                    count, size);
            } else {
                // Valid allocation - should succeed or fail gracefully
                // (We don't assert success as memory may be exhausted)
                if (result != NULL) {
                    free(result);  // Clean up if allocation succeeded
                }
            }
        } else {
            // Zero-sized allocation - implementation defined, but shouldn't crash
            if (result != NULL) {
                free(result);
            }
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_allocation_size_overflow_protection);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}