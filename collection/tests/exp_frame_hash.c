/*
 * exp_frame_hash.c — Frame-Seek Driven Hash + Random Set Experiments
 *
 * ─── CONFIG ────────────────────────────────────────────────────
 * 3456     = SEED       hash mixing seed
 * 8        = LUT_SIZE   LUT entry count (hash table slots)
 * 5        = SET_COUNT  number of output sets
 * 256      = RV_RANGE   0..255 byte range
 * 2        = RV_MODE    0=fixed, 1=coupon, 2=permute
 * 1        = PAYLINE    payline match printf flag
 * 25       = RTP_PCT    RTP % (0-100) — higher = more wins
 * 1        = NEAR_MISS  0=off, 1=show near-miss reels
 * ──────────────────────────────────────────────────────────────*/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "geo_frame_seek.h"

#define SEED            3456u
#define LUT_SIZE        256u
#define SET_COUNT       5u
#define RV_RANGE        256u
#define RV_MODE         1u       /* 0=fixed, 1=coupon, 2=permute */

#define PAYLINE         1u       /* 0=off, 1=show paylines */
#define PAY_MATCH       16u      /* symbol % N == 0 = match */
#define RTP_PCT         25u      /* 0-100: win rate injected via mapping */
#define NEAR_MISS       1u       /* 0=off, 1=show near-miss reels */

/* payout tier from raw byte + injected RTP bias */
static const char *payout_name(uint8_t raw, uint32_t lose_count) {
    uint32_t bias = lose_count / 3u;         /* hunger: every 3 losses → +1% win */
    uint32_t threshold = 255u - RTP_PCT - bias;
    if (threshold > 255) threshold = 0;
    if (raw >= threshold) return "JACKPOT";
    if (raw >= threshold - 20u) return "NEAR";
    if (raw >= threshold - 50u) return "MINOR";
    return "LOSE";
}

static uint32_t mix32(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = SEED;
    h ^= a * 0x9e3779b9u;
    h ^= b * 0x85ebca6bu;
    h ^= c * 0xc2b2ae35u;
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static uint8_t frame_byte(uint32_t t) {
    DualFrame f = frame_seek(t);
    return (uint8_t)(mix32(f.ico_idx, f.phase * 37u, f.slot) & 0xFF);
}

int main(void) {
    printf("=== Frame-Seek LUT + RV (seed=%u, mode=%u, range=%u) ===\n\n",
           SEED, RV_MODE, RV_RANGE);

    /* ── 1. LUT ── */
    uint32_t lut[LUT_SIZE];
    printf("[LUT] %u-entry hash table:\n\n", LUT_SIZE);
    printf("  i | enc  fce slt phs  hash32   | b3  b2  b1  b0 | row bytes\n");
    printf("  --+------------------------------+----------------+--------------------------\n");
    for (uint32_t i = 0; i < LUT_SIZE; i++) {
        DualFrame f = frame_seek(i);
        lut[i] = mix32(f.face, f.slot, f.phase);
        uint8_t b3 = (uint8_t)(lut[i] >> 24);
        uint8_t b2 = (uint8_t)(lut[i] >> 16);
        uint8_t b1 = (uint8_t)(lut[i] >>  8);
        uint8_t b0 = (uint8_t)(lut[i] >>  0);
        printf("  %1u | %04u  %02u  %03u   %02u  0x%08X | %02x %02x %02x %02x | %03u %03u %03u %03u\n",
               i, f.enc, f.face, f.slot, f.phase, lut[i],
               b3, b2, b1, b0, b3, b2, b1, b0);
    }

    /* verify: decode row bytes → hash32 */
    printf("\n  [verify] decode row bytes back to hash32:\n");
    for (uint32_t i = 0; i < LUT_SIZE; i++) {
        uint8_t b3 = (uint8_t)(lut[i] >> 24);
        uint8_t b2 = (uint8_t)(lut[i] >> 16);
        uint8_t b1 = (uint8_t)(lut[i] >>  8);
        uint8_t b0 = (uint8_t)(lut[i] >>  0);
        uint32_t dec = ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16)
                     | ((uint32_t)b1 <<  8) | ((uint32_t)b0 <<  0);
        printf("    [%1u] 0x%08X -> bytes %02x %02x %02x %02x -> 0x%08X %s\n",
               i, lut[i], b3, b2, b1, b0, dec, dec == lut[i] ? "✓" : "✗ MISMATCH");
    }

    /* ── 2. Sets ── */
    printf("\n[RV] %u set(s), range 0..%u, mode=%u:\n\n",
           SET_COUNT, RV_RANGE - 1, RV_MODE);

#if RV_MODE == 0
    /* FIXED: pure random, fixed length = RV_RANGE */
    for (uint32_t s = 0; s < SET_COUNT; s++) {
        printf("  Set %u (%u vals):", s, RV_RANGE);
        for (uint32_t j = 0; j < RV_RANGE; j++)
            printf(" %s%u", (j % 16 == 0) ? "\n   " : "", frame_byte(s * 10000u + j));
        printf("\n");
    }

#elif RV_MODE == 1
    /* COUPON: track seen bitset, collect until all found */
    for (uint32_t s = 0; s < SET_COUNT; s++) {
        uint8_t seen[RV_RANGE / 8] = {0};
        uint32_t found = 0, draws = 0, collected[RV_RANGE];
        while (found < RV_RANGE) {
            uint8_t v = frame_byte(s * 100000u + draws);
            uint32_t word_idx = v / 8, bit_idx = v % 8;
            if (!(seen[word_idx] & (1u << bit_idx))) {
                seen[word_idx] |= (1u << bit_idx);
                collected[found++] = v;
            }
            draws++;
            if (draws > 1000000u) break; /* safety */
        }
        printf("  Set %u: %u draws to collect all %u values\n", s, draws, RV_RANGE);
        printf("   collected:");
        for (uint32_t j = 0; j < RV_RANGE; j++)
            printf(" %s%u", (j % 16 == 0) ? "\n    " : "", collected[j]);
        printf("\n\n");
    }

#else /* RV_MODE == 2 */
    /* PERMUTE: Fisher-Yates shuffle of 0..RV_RANGE-1 driven by frame_seek */
    for (uint32_t s = 0; s < SET_COUNT; s++) {
        uint16_t perm[RV_RANGE];
        for (uint32_t j = 0; j < RV_RANGE; j++) perm[j] = (uint16_t)j;
        for (uint32_t j = RV_RANGE - 1; j > 0; j--) {
            DualFrame f = frame_seek(s * 100000u + j);
            uint32_t idx = mix32(f.face, f.slot, f.phase) % (j + 1);
            uint16_t tmp = perm[j]; perm[j] = perm[idx]; perm[idx] = tmp;
        }
        printf("  Set %u (%u vals, all distinct):\n", s, RV_RANGE);
        for (uint32_t j = 0; j < RV_RANGE; j++) {
            if (j % 16 == 0) printf("   ");
            printf("%03u%s", perm[j], (j % 16 == 15) ? "\n" : " ");
        }
        printf("\n");
    }
#endif

#if PAYLINE
    /* ── 3. Slot machine: payline with RTP mapping layer ── */
    printf("\n[PAYLINE] RTP=%u%%, near-miss=%s\n", RTP_PCT,
           NEAR_MISS ? "ON" : "OFF");
    printf("  mapping: raw byte + lose_count → payout tier\n\n");

    for (uint32_t s = 0; s < 5; s++) {
        uint8_t r[5][3];
        uint32_t base = s * 200u;
        uint32_t lose_streak = 0;

        printf("  ───── Spin %u ─────\n", s);
        printf("  reel:   0     1     2     3     4\n");
        printf("  ─────────────────────────────────\n");
        for (uint32_t row = 0; row < 3; row++) {
            printf("  row%u: ", row);
            for (uint32_t col = 0; col < 5; col++) {
                uint32_t t = base + col * 3u + row;
                r[col][row] = frame_byte(t);
                const char *tier = payout_name(r[col][row], lose_streak);
                if (tier[0] == 'J') lose_streak = 0;
                else                lose_streak++;
                char mark = ' ';
                if      (tier[0] == 'J') mark = '$';
                else if (tier[0] == 'N') mark = NEAR_MISS ? '~' : ' ';
                else if (tier[0] == 'M' && tier[1] == 'I') mark = '*';
                printf("%03u%c ", r[col][row], mark);
            }
            printf("\n");
        }
        printf("  lose_streak=%u\n", lose_streak);
        printf("\n");
    }

    /* show RTP vs raw threshold table */
    printf("\n  [RTP MAP] raw byte → payout tier (at lose_count=0):\n");
    printf("    %3u-255 → JACKPOT  (top %u%%)\n",
           256 - RTP_PCT, RTP_PCT);
    printf("    %3u-%3u → NEAR\n",
           256 - RTP_PCT - 20, 256 - RTP_PCT - 1);
    printf("    %3u-%3u → MINOR\n",
           256 - RTP_PCT - 50, 256 - RTP_PCT - 21);
    printf("      0-%3u → LOSE\n\n", 256 - RTP_PCT - 51);
#endif

    printf("[OK] deterministic — seed=%u re-run gives identical output.\n", SEED);
    return 0;
}
