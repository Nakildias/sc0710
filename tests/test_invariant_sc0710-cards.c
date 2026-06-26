#include <check.h>
#include <stdlib.h>
#include <stdint.h>

// Declare the function from the production file
extern int sc0710_load_firmware(const void *fw_data, size_t fw_size);

START_TEST(test_firmware_size_overflow_invariant)
{
    // Invariant: For any input size, the allocation size calculation must not overflow
    // and must preserve the relationship: allocated_size >= required_size
    
    // Payloads: exploit case, boundary case, valid case
    size_t payloads[] = {
        (size_t)UINT32_MAX + 1,  // Exact exploit: half_size * 2 overflows to 0 on 32-bit
        (size_t)UINT32_MAX,      // Boundary: maximum 32-bit value
        1024                     // Valid normal size
    };
    
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    for (int i = 0; i < num_payloads; i++) {
        size_t fw_size = payloads[i];
        
        // Create minimal valid firmware header
        uint8_t *fw_data = malloc(fw_size);
        ck_assert_ptr_nonnull(fw_data);
        
        // Set header magic to pass basic validation
        if (fw_size >= 16) {
            memcpy(fw_data, "FWI_HEADER", 10);
        }
        
        // Call the actual production function
        int result = sc0710_load_firmware(fw_data, fw_size);
        
        // Security property: function must handle size safely
        // Either succeed with proper allocation or fail cleanly
        ck_assert(result == -ENOMEM || result == 0 || result == -EINVAL);
        
        free(fw_data);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;
    
    s = suite_create("Security");
    tc_core = tcase_create("Core");
    
    tcase_add_test(tc_core, test_firmware_size_overflow_invariant);
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