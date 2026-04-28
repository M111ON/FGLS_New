/*
 * GeoPixel Ultimate Lossless Image Codec v2
 * Fixed decoder with proper state management
 * * Key fixes:
 * - Decoder now maintains separate Y/CG/CO reconstruction buffers
 * - Prediction uses reconstructed transform values, not RGB
 * - Proper integer arithmetic throughout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define MAX_WIDTH 4096
#define MAX_HEIGHT 4096

// BMP Header structures
#pragma pack(push, 1)
typedef struct {
    uint16_t signature;
    uint32_t fileSize;
    uint32_t reserved;
    uint32_t dataOffset;
} BMPHeader;

typedef struct {
    uint32_t headerSize;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bitCount;
    uint32_t compression;
    uint32_t imageSize;
    int32_t xPelsPerMeter;
    int32_t yPelsPerMeter;
    uint32_t colorsUsed;
    uint32_t colorsImportant;
} DIBHeader;
#pragma pack(pop)

// Bit writer for entropy coding
typedef struct {
    uint8_t* buffer;
    size_t capacity;
    size_t bitPos;
} BitWriter;

void bw_init(BitWriter* bw, size_t capacity) {
    bw->buffer = calloc(capacity, 1);
    bw->capacity = capacity;
    bw->bitPos = 0;
}

void bw_writeBit(BitWriter* bw, int bit) {
    if (bw->bitPos >= bw->capacity * 8) return;
    int byteIdx = bw->bitPos / 8;
    int bitIdx = 7 - (bw->bitPos % 8);
    if (bit) bw->buffer[byteIdx] |= (1 << bitIdx);
    bw->bitPos++;
}

void bw_writeGolomb(BitWriter* bw, int value) {
    // Map signed to unsigned: 0->0, -1->1, 1->2, -2->3, 2->4...
    uint32_t uval = (value <= 0) ? (-value * 2) : (value * 2 - 1);
    
    // Golomb with k=0 (unary coding for small values)
    for (uint32_t i = 0; i < uval && bw->bitPos < bw->capacity * 8; i++) {
        bw_writeBit(bw, 0);
    }
    bw_writeBit(bw, 1);
}

size_t bw_getBytes(BitWriter* bw) {
    return (bw->bitPos + 7) / 8;
}

// Bit reader for decoding
typedef struct {
    uint8_t* buffer;
    size_t size;
    size_t bitPos;
} BitReader;

void br_init(BitReader* br, uint8_t* buffer, size_t size) {
    br->buffer = buffer;
    br->size = size;
    br->bitPos = 0;
}

int br_readBit(BitReader* br) {
    if (br->bitPos >= br->size * 8) return 0;
    int byteIdx = br->bitPos / 8;
    int bitIdx = 7 - (br->bitPos % 8);
    br->bitPos++;
    return (br->buffer[byteIdx] >> bitIdx) & 1;
}

int br_readGolomb(BitReader* br) {
    int zeros = 0;
    while (br_readBit(br) == 0) {
        zeros++;
    }
    // Map back to signed
    return (zeros % 2 == 0) ? -(zeros / 2) : ((zeros + 1) / 2);
}

// Reversible YCgCo-R color transform (exact integer math)
static inline void rgb_to_ycgco(int r, int g, int b, int* y, int* cg, int* co) {
    *co = r - b;
    int tmp = b + (*co) / 2;
    *cg = g - tmp;
    *y = tmp + (*cg) / 2;
}

static inline void ycogco_to_rgb(int y, int cg, int co, int* r, int* g, int* b) {
    int tmp = y - cg / 2;
    *g = cg + tmp;
    *b = tmp - co / 2;
    *r = *b + co;
}

static inline int clamp(int v, int min, int max) {
    return v < min ? min : (v > max ? max : v);
}

// MED predictor
static inline int med_predict(int a, int b, int c) {
    if (c >= a && c >= b) return a + b - c;
    if (c <= a && c <= b) return a + b - c;
    return c;
}

int encode_bmp(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) { perror("fopen"); return -1; }
    
    BMPHeader bmpHdr;
    DIBHeader dibHdr;
    fread(&bmpHdr, sizeof(bmpHdr), 1, f);
    fread(&dibHdr, sizeof(dibHdr), 1, f);
    
    if (bmpHdr.signature != 0x4D42 || dibHdr.bitCount != 24) {
        printf("Error: Not a 24-bit BMP\n");
        fclose(f);
        return -1;
    }
    
    int W = dibHdr.width, H = dibHdr.height;
    int rowPad = (4 - (W * 3) % 4) % 4;
    
    uint8_t* pixels = malloc(W * H * 3);
    fseek(f, bmpHdr.dataOffset, SEEK_SET);
    
    for (int y = 0; y < H; y++) {
        uint8_t* row = &pixels[(H - 1 - y) * W * 3];
        fread(row, W * 3, 1, f);
        fseek(f, rowPad, SEEK_CUR);
    }
    fclose(f);
    
    // Convert to YCgCo
    int* Y = malloc(W * H * sizeof(int));
    int* CG = malloc(W * H * sizeof(int));
    int* CO = malloc(W * H * sizeof(int));
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = (y * W + x) * 3;
            int r = pixels[idx + 2];
            int g = pixels[idx + 1];
            int b = pixels[idx + 0];
            rgb_to_ycgco(r, g, b, &Y[y*W+x], &CG[y*W+x], &CO[y*W+x]);
        }
    }
    
    // Compute residuals with MED prediction
    int* resY = malloc(W * H * sizeof(int));
    int* resCG = malloc(W * H * sizeof(int));
    int* resCO = malloc(W * H * sizeof(int));
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int left_y = (x > 0) ? Y[y*W + x-1] : 128;
            int top_y = (y > 0) ? Y[(y-1)*W + x] : 128;
            int tl_y = (x > 0 && y > 0) ? Y[(y-1)*W + x-1] : 128;
            resY[y*W+x] = Y[y*W+x] - med_predict(left_y, top_y, tl_y);
            
            int left_cg = (x > 0) ? CG[y*W + x-1] : 0;
            int top_cg = (y > 0) ? CG[(y-1)*W + x] : 0;
            int tl_cg = (x > 0 && y > 0) ? CG[(y-1)*W + x-1] : 0;
            resCG[y*W+x] = CG[y*W+x] - med_predict(left_cg, top_cg, tl_cg);
            
            int left_co = (x > 0) ? CO[y*W + x-1] : 0;
            int top_co = (y > 0) ? CO[(y-1)*W + x] : 0;
            int tl_co = (x > 0 && y > 0) ? CO[(y-1)*W + x-1] : 0;
            resCO[y*W+x] = CO[y*W+x] - med_predict(left_co, top_co, tl_co);
        }
    }
    
    // Entropy code
    size_t estSize = W * H * 4;
    BitWriter bwY, bwCG, bwCO;
    bw_init(&bwY, estSize);
    bw_init(&bwCG, estSize);
    bw_init(&bwCO, estSize);
    
    for (int i = 0; i < W * H; i++) {
        bw_writeGolomb(&bwY, resY[i]);
        bw_writeGolomb(&bwCG, resCG[i]);
        bw_writeGolomb(&bwCO, resCO[i]);
    }
    
    size_t sizeY = bw_getBytes(&bwY);
    size_t sizeCG = bw_getBytes(&bwCG);
    size_t sizeCO = bw_getBytes(&bwCO);
    size_t totalSize = sizeY + sizeCG + sizeCO + 20;
    
    // Write output
    char outName[256];
    snprintf(outName, sizeof(outName), "%s.ultimate", filename);
    FILE* out = fopen(outName, "wb");
    
    fwrite(&W, sizeof(int), 1, out);
    fwrite(&H, sizeof(int), 1, out);
    fwrite(&sizeY, sizeof(size_t), 1, out);
    fwrite(&sizeCG, sizeof(size_t), 1, out);
    fwrite(&sizeCO, sizeof(size_t), 1, out);
    
    fwrite(bwY.buffer, 1, sizeY, out);
    fwrite(bwCG.buffer, 1, sizeCG, out);
    fwrite(bwCO.buffer, 1, sizeCO, out);
    
    fclose(out);
    
    // DECODE AND VERIFY (using separate Y/CG/CO buffers for prediction)
    BitReader brY, brCG, brCO;
    br_init(&brY, bwY.buffer, sizeY);
    br_init(&brCG, bwCG.buffer, sizeCG);
    br_init(&brCO, bwCO.buffer, sizeCO);
    
    int* decY = malloc(W * H * sizeof(int));
    int* decCG = malloc(W * H * sizeof(int));
    int* decCO = malloc(W * H * sizeof(int));
    uint8_t* decoded = malloc(W * H * 3);
    
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int res_y = br_readGolomb(&brY);
            int res_cg = br_readGolomb(&brCG);
            int res_co = br_readGolomb(&brCO);
            
            // Predict using RECONSTRUCTED Y/CG/CO values (not RGB!)
            int left_y = (x > 0) ? decY[y*W + x-1] : 128;
            int top_y = (y > 0) ? decY[(y-1)*W + x] : 128;
            int tl_y = (x > 0 && y > 0) ? decY[(y-1)*W + x-1] : 128;
            decY[y*W+x] = res_y + med_predict(left_y, top_y, tl_y);
            
            int left_cg = (x > 0) ? decCG[y*W + x-1] : 0;
            int top_cg = (y > 0) ? decCG[(y-1)*W + x] : 0;
            int tl_cg = (x > 0 && y > 0) ? decCG[(y-1)*W + x-1] : 0;
            decCG[y*W+x] = res_cg + med_predict(left_cg, top_cg, tl_cg);
            
            int left_co = (x > 0) ? decCO[y*W + x-1] : 0;
            int top_co = (y > 0) ? decCO[(y-1)*W + x] : 0;
            int tl_co = (x > 0 && y > 0) ? decCO[(y-1)*W + x-1] : 0;
            decCO[y*W+x] = res_co + med_predict(left_co, top_co, tl_co);
            
            // Convert back to RGB
            int r, g, b;
            ycogco_to_rgb(decY[y*W+x], decCG[y*W+x], decCO[y*W+x], &r, &g, &b);
            
            decoded[(y*W + x) * 3 + 0] = clamp(b, 0, 255);
            decoded[(y*W + x) * 3 + 1] = clamp(g, 0, 255);
            decoded[(y*W + x) * 3 + 2] = clamp(r, 0, 255);
        }
    }
    
    // Calculate PSNR
    double mse = 0;
    for (int i = 0; i < W * H * 3; i++) {
        int diff = pixels[i] - decoded[i];
        mse += diff * diff;
    }
    mse /= (W * H * 3);
    double psnr = (mse == 0) ? 999.0 : 10 * log10(255.0 * 255.0 / mse);
    
    printf("Image: %s (%dx%d)\n", filename, W, H);
    printf("\n=== Streams ===\n");
    printf("  [Y ] encoded=%zuB  (%.3f bpp)\n", sizeY, sizeY * 8.0 / (W * H));
    printf("  [CG] encoded=%zuB  (%.3f bpp)\n", sizeCG, sizeCG * 8.0 / (W * H));
    printf("  [CO] encoded=%zuB  (%.3f bpp)\n", sizeCO, sizeCO * 8.0 / (W * H));
    printf("\n=== Results ===\n");
    printf("  Raw     : %d B  (1.00x)\n", W * H * 3);
    printf("  Encoded : %zu B  (%.2fx)\n", totalSize, (double)(W * H * 3) / totalSize);
    printf("  Lossless: %s\n", memcmp(pixels, decoded, W * H * 3) == 0 ? "YES ✓" : "NO ✗");
    printf("  PSNR    : %.2f dB\n", psnr);
    
    free(pixels);
    free(Y); free(CG); free(CO);
    free(resY); free(resCG); free(resCO);
    free(decY); free(decCG); free(decCO);
    free(decoded);
    free(bwY.buffer); free(bwCG.buffer); free(bwCO.buffer);
    
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <image.bmp>\n", argv[0]);
        return 1;
    }
    
    clock_t start = clock();
    encode_bmp(argv[1]);
    clock_t end = clock();
    
    printf("  Time    : %.2f ms\n", (double)(end - start) / CLOCKS_PER_SEC * 1000);
    
    return 0;
}