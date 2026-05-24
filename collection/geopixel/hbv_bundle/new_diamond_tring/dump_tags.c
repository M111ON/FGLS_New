#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);

    /* Skip header 64B + zone_count 4B + zones (read count) + n_chunks 4B */
    int off = 64;
    int n_zones = 0; memcpy(&n_zones, buf + off, 4); off += 4;
    off += n_zones * 8; /* ZoneRecord = 8B */
    int n_chunks = 0; memcpy(&n_chunks, buf + off, 4); off += 4;

    printf("File: %s  total=%ld  n_zones=%d  n_chunks=%d  payload_at=%d\n",
           argv[1], sz, n_zones, n_chunks, off);

    int seq = 0;
    while (off < (int)sz && seq < n_chunks) {
        uint8_t tag = buf[off];
        int skip;
        switch (tag) {
        case 0: skip = 1; printf("seq=%d tag=0  FLAT     1B\n", seq); break;
        case 1: skip = 65; printf("seq=%d tag=1  RAW     65B\n", seq); break;
        case 2: { uint32_t dsz; if (off + 5 <= sz) memcpy(&dsz, buf+off+1, 4); else dsz=0; skip = 5 + dsz; printf("seq=%d tag=2  DIAMOND dsz=%u %uB\n", seq, dsz, 5+dsz); break; }
        case 3: skip = 5; printf("seq=%d tag=3  BATCH   ref=%u 5B\n", seq, *(uint32_t*)(buf+off+1)); break;
        case 4: skip = 5; printf("seq=%d tag=4  GEOM    ref=%u 5B\n", seq, *(uint32_t*)(buf+off+1)); break;
        case 5: skip = 1; printf("seq=%d tag=5  IDENT   1B\n", seq); break;
        case 6: skip = 2; printf("seq=%d tag=6  BROT    k=%u 2B\n", seq, buf[off+1]); break;
        case 7: skip = 1; printf("seq=%d tag=7  BREF    1B\n", seq); break;
        case 8: skip = 2; printf("seq=%d tag=8  D4      i=%u 2B\n", seq, buf[off+1]); break;
        case 9: skip = 10 + buf[off+1]; printf("seq=%d tag=9  DIFF    cnt=%u %dBYTES\n", seq, buf[off+1], skip); break;
        default: printf("seq=%d UNKNOWN tag=%d at offset %d\n", seq, tag, off); skip = 1; break;
        }
        off += skip;
        seq++;
    }
    printf("Total seq=%d, remaining bytes=%d\n", seq, (int)sz - off);
    free(buf);
    return 0;
}
