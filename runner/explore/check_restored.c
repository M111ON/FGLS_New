#include <stdio.h>
#include <stdint.h>
int main(void) {
    FILE *f = fopen("I:/model/Qwen3-0.6B-Q8_0.restored.gguf", "rb");
    if (!f) return 1;
    uint8_t hdr[16];
    fread(hdr, 1, 16, f);
    printf("First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],hdr[5],hdr[6],hdr[7]);
    printf("Expected GGUF: 47 55 47 46 03 00 00 00\n");
    printf("Magic: %s\n", (hdr[0]=='G' && hdr[1]=='G' && hdr[2]=='U' && hdr[3]=='F') ? "OK" : "BAD - header not copied!");
    fclose(f);
    return 0;
}
