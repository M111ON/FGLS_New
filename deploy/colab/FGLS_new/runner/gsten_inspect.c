#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tw_face_bridge.h"
#include "tw_tensor_capture.h"

#define STNG_MAGIC 0x474E5453u

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: gsten_inspect <file.gsten> [--csv]\n");
        return 1;
    }
    int csv_mode = (argc >= 3 && strcmp(argv[2], "--csv") == 0);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", argv[1]); return 1; }

    uint32_t magic, version, n_captured, n_freeze;
    char ts[32];
    if (fread(&magic,4,1,f)!=1||fread(&version,4,1,f)!=1||
        fread(&n_captured,4,1,f)!=1||fread(&n_freeze,4,1,f)!=1||
        fread(ts,32,1,f)!=1) {
        fprintf(stderr, "Bad header\n"); fclose(f); return 1;
    }
    if (magic != STNG_MAGIC) {
        fprintf(stderr, "Not a gsten file (magic=0x%08X)\n", magic);
        fclose(f); return 1;
    }

    fprintf(stderr, "=== Gsten: %s ===\n", argv[1]);
    fprintf(stderr, "%u tensors, %u freeze entries, version=%u, time=%.32s\n",
            n_captured, n_freeze, version, ts);

    if (csv_mode) {
        printf("tensor_name,node_id,shell,face,sig_x,sig_y\n");
    } else {
        printf("%-5s %-50s %-8s %-7s %-6s %-8s %-8s\n",
               "idx", "tensor_name", "node_id", "shell", "face", "sig_x", "sig_y");
    }

    for (uint32_t i = 0; i < n_captured; i++) {
        uint16_t nlen;
        if (fread(&nlen,2,1,f)!=1) { fprintf(stderr, "truncated at %u\n", i); break; }
        char name[256];
        if (nlen >= 256) nlen = 255;
        if (fread(name,1,nlen,f) != nlen) break;
        name[nlen] = 0;

        uint8_t dtype = 8; /* v1 default */
        if (version >= 2) {
            if (fread(&dtype,1,1,f) != 1) break;
        }

        uint64_t dsz;
        if (fread(&dsz,8,1,f) != 1) break;

        uint8_t *data = (uint8_t*)malloc(dsz);
        if (!data) { fprintf(stderr, "OOM at tensor %u\n", i); break; }
        if (fread(data, 1, dsz, f) != dsz) { free(data); break; }

        TWTensorCapture tc;
        tw_capture_tensor_raw(data, dsz, 0, 0, dtype, &tc);
        uint32_t node_id = tw_to_node(tc.cap.zone, tc.cap.slot);
        uint32_t shell = node_id / 1728;
        uint32_t face = shell % 12;

        if (csv_mode) {
            printf("%s,%u,%u,%u,%.2f,%.2f\n",
                   name, node_id, shell, face, tc.sig_x, tc.sig_y);
        } else {
            printf("%-5u %-50s %-8u %-7u %-6u %-8.2f %-8.2f\n",
                   i, name, node_id, shell, face, tc.sig_x, tc.sig_y);
        }
        free(data);
    }

    fclose(f);
    return 0;
}
