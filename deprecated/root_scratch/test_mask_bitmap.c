// test_mask_bitmap.c
// Quick test: write sparse values, read back, show size savings.

#include <stdio.h>
#include "mask_bitmap.h"

int main(void) {
    mb_field_t field;
    mb_init(&field);

    mb_mask_t *m0 = &field.masks[0];

    // simulate loading some weights into random-ish slots
    mb_set(m0, 5,   1.1f);
    mb_set(m0, 42,  2.2f);
    mb_set(m0, 999, 3.3f);
    mb_set(m0, 100, 4.4f);
    mb_set(m0, 5,   9.9f);  // overwrite existing slot

    mb_value_t v;

    printf("slot 5   -> %s\n", mb_get(m0, 5, &v) == 0 ? "found" : "missing");
    printf("  value = %.2f (expect 9.90)\n", v);

    printf("slot 42  -> %s\n", mb_get(m0, 42, &v) == 0 ? "found" : "missing");
    printf("  value = %.2f (expect 2.20)\n", v);

    printf("slot 7   -> %s (expect missing)\n", mb_get(m0, 7, &v) == 0 ? "found" : "missing");

    mb_clear(m0, 42);
    printf("after clear, slot 42 -> %s (expect missing)\n", mb_get(m0, 42, &v) == 0 ? "found" : "missing");

    printf("\nactive_count = %u\n", m0->active_count);
    printf("bitmap bytes/mask = %zu\n", mb_bitmap_bytes_per_mask());
    printf("payload bytes (this mask) = %zu\n", mb_payload_bytes(m0));
    printf("total bytes (this mask) = %zu\n", mb_total_bytes(m0));

    size_t dense_bytes = sizeof(mb_value_t) * MB_SLOTS_PER_MASK;
    printf("\nvs dense array (%d slots) = %zu bytes\n", MB_SLOTS_PER_MASK, dense_bytes);
    printf("savings this mask: %.1f%%\n",
           100.0 * (1.0 - (double)mb_total_bytes(m0) / (double)dense_bytes));

    return 0;
}
