#include <stdio.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: hbv_debug <file.gpx5>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 1; }

    uint8_t hdr[24];
    size_t n = fread(hdr, 1, 24, f);
    fclose(f);

    printf("bytes read: %zu\n", n);
    printf("raw bytes: ");
    for (int i = 0; i < (int)n; i++) printf("%02X ", hdr[i]);
    printf("\n");

    uint32_t magic   = hdr[0]|(hdr[1]<<8)|((uint32_t)hdr[2]<<16)|((uint32_t)hdr[3]<<24);
    uint8_t  version = hdr[4];
    uint16_t tw      = hdr[8]|(hdr[9]<<8);
    uint16_t th      = hdr[10]|(hdr[11]<<8);
    uint32_t n_tiles = hdr[12]|(hdr[13]<<8)|((uint32_t)hdr[14]<<16)|((uint32_t)hdr[15]<<24);

    printf("magic=0x%08X (expect 0x47505835)\n", magic);
    printf("version=%d\n", version);
    printf("tw=%d th=%d\n", tw, th);
    printf("n_tiles=%u\n", n_tiles);

    int tiles_x = 256 / 16;  /* HBV_IMAGE_W / HBV_TILE_W */
    int tiles_y = (int)((n_tiles + tiles_x - 1) / tiles_x);
    int img_h   = tiles_y * 16;
    printf("computed img_h=%d\n", img_h);
    return 0;
}
