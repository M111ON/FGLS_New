#include <stdio.h>
#include <string.h>
#include "tensor_memory.h"

static int pass = 0, fail = 0;
#define CHECK(label, cond) do { if (cond) { pass++; printf("  PASS  %s\n", label); } else { printf("  FAIL  %s\n", label); fail++; } } while(0)

static int counting_cb(const ZoneCardSID *z, const char *name, uint32_t idx, void *ctx)
{
    (void)z; (void)name; (void)idx;
    (*(int*)ctx)++;
    return 0;
}

int main(void)
{
    printf("tensor_memory.h test\n");

    /* T1: struct sizes */
    CHECK("ZoneCardSID == 42B", sizeof(ZoneCardSID) == 42);
    CHECK("TensorMemHeader == 32B", sizeof(TensorMemHeader) == 32);

    /* T2: init */
    uint8_t buf[65536];
    TensorMemStore s;
    tmem_init(&s, buf, sizeof(buf), 100);
    CHECK("init used == 32", s.used == 32);
    CHECK("init n_records == 0", s.n_records == 0);

    /* T3: append raw */
    ZoneCardSID z1 = {0};
    z1.node_id = 42;
    z1.inf.logit_entropy = 128;
    z1.inf.tick = 101;
    z1.card.card_type = 0;
    uint8_t data1[64];
    for (int i = 0; i < 64; i++) data1[i] = (uint8_t)i;
    int r = tmem_append_raw(&s, &z1, "test_tensor", data1, 64);
    CHECK("append raw ok", r == 0);
    CHECK("n_records == 1", s.n_records == 1);

    /* T4: append delta (2 bytes differ from baseline) */
    ZoneCardSID z2 = {0};
    z2.node_id = 99;
    z2.inf.logit_entropy = 200;
    z2.inf.tick = 102;
    z2.card.card_type = 1;
    uint8_t data2[64], base2[64];
    for (int i = 0; i < 64; i++) { base2[i] = (uint8_t)i; data2[i] = (uint8_t)i; }
    data2[3] = 0xFF; data2[17] = 0xAA;
    r = tmem_append_delta(&s, &z2, "delta_tensor", data2, 64, base2);
    CHECK("append delta ok", r == 0);
    CHECK("n_records == 2", s.n_records == 2);

    /* T5: read back raw */
    uint8_t readback[64];
    ZoneCardSID zr;
    r = tmem_read_record(&s, 0, readback, 64, &zr, NULL);
    CHECK("read record 0 ok", r == 64);
    CHECK("node_id = 42", zr.node_id == 42);
    CHECK("entropy = 128", zr.inf.logit_entropy == 128);
    CHECK("raw data matches", memcmp(readback, data1, 64) == 0);

    /* T6: read back delta */
    memset(readback, 0, 64);
    r = tmem_read_record(&s, 1, readback, 64, &zr, base2);
    CHECK("read record 1 ok", r == 64);
    CHECK("node_id = 99", zr.node_id == 99);
    CHECK("entropy = 200", zr.inf.logit_entropy == 200);
    CHECK("delta decoded matches", memcmp(readback, data2, 64) == 0);

    /* T7: get ZCSID without data */
    const ZoneCardSID *zp = tmem_record_zcsid(&s, 0);
    CHECK("zcsid ptr not null", zp != NULL);
    CHECK("zcsid node_id match", zp->node_id == 42);

    /* T8: query */
    uint32_t matches[8];
    TensorMemQuery q = {0};

    q.mask = TMEM_FILTER_ENTROPY; q.entropy_min = 150; q.entropy_max = 255;
    int n = tmem_query(&s, &q, matches, 8);
    CHECK("query high entropy finds 1", n == 1);
    CHECK("match idx = 1", n > 0 && matches[0] == 1);

    memset(&q, 0, sizeof(q));
    n = tmem_query(&s, &q, matches, 8);
    CHECK("query no filter finds 2", n == 2);

    memset(&q, 0, sizeof(q)); q.mask = TMEM_FILTER_NODE_ID; q.node_id = 42;
    n = tmem_query(&s, &q, matches, 8);
    CHECK("query node_id=42 finds 1", n == 1);

    /* T9: save + load roundtrip */
    r = tmem_save(&s, "test_tmem.bin");
    CHECK("save ok", r == 0);

    uint8_t load_buf[65536];
    TensorMemStore s2;
    r = tmem_load(&s2, load_buf, sizeof(load_buf), "test_tmem.bin");
    CHECK("load ok", r == 0);
    CHECK("n_records match", s2.n_records == 2);
    CHECK("tick_base match", s2.tick_base == 100);

    r = tmem_read_record(&s2, 0, readback, 64, &zr, NULL);
    CHECK("load verify raw", r == 64 && zr.node_id == 42);
    r = tmem_read_record(&s2, 1, readback, 64, &zr, base2);
    CHECK("load verify delta", r == 64 && zr.node_id == 99);

    /* T10: foreach */
    int count = 0;
    r = tmem_foreach(&s, counting_cb, &count);
    CHECK("foreach ok", r == 0);
    CHECK("foreach count == 2", count == 2);

    /* T11: capacity estimate */
    uint32_t cap = tmem_capacity(65536, 64, 10);
    CHECK("capacity estimate > 0", cap > 0);

    remove("test_tmem.bin");

    printf("\nResult: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
