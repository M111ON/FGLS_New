/*
 * addr_resolve.c — Tensor name → 144² address resolution
 *
 * Usage: addr_resolve --name "blk.0.attn_q.weight" [--tier N]
 *        addr_resolve --file model.gguf [--tier N]
 *
 * Shows address, decomposition, and geometry for tensor names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_core.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --name \"tensor.name\" [--tier N]\n", prog);
    fprintf(stderr, "       %s --file model.gguf [--tier N]\n", prog);
    fprintf(stderr, "  --name   Resolve single tensor name\n");
    fprintf(stderr, "  --file   List all tensors in GGUF file\n");
    fprintf(stderr, "  --tier   Address tier (default: 0)\n");
}

static void resolve_name(const char *name, uint8_t tier) {
    uint32_t addr = pogls_addr_from_name(name, tier);
    PoglsAddrDecomp d = pogls_addr_decompose(addr, tier);

    printf("Tensor:   %s\n", name);
    printf("Address:  %u (0x%04X)\n", addr, addr);
    printf("Tier:     %u (%s)\n", tier, pogls_addr_tier_name(tier));
    printf("Macro:    %u / %u\n", d.macro, POGLS_ADDR_TIERS[tier].macro_slots);
    printf("Micro:    %u / %u\n", d.micro, POGLS_ADDR_TIERS[tier].micro_slots);
    printf("Valid:    %s\n", pogls_addr_valid(addr, tier) ? "YES" : "NO");

    /* Show face rotation */
    printf("\nFace rotation:\n");
    for (int f = 0; f < 6; f++) {
        uint32_t face_addr = pogls_addr_capo(addr, (uint32_t)f, tier);
        printf("  face %d: %u (0x%04X)\n", f, face_addr, face_addr);
    }
    printf("\n");
}

/* GGUF type names */
static const char* gguf_type_name(uint32_t t) {
    switch (t) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 9:  return "Q8_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        case 20: return "IQ4_NL";
        case 23: return "IQ4_XS";
        case 30: return "BF16";
        default: return "???";
    }
}

#include "gguf_reader.h"

static void resolve_file(const char *path, uint8_t tier) {
    GgufReader reader;
    if (gguf_open(path, &reader) != 0) {
        fprintf(stderr, "Error: cannot open GGUF file %s\n", path);
        return;
    }

    printf("═══ GGUF Address Resolution ═══\n");
    printf("File:     %s\n", path);
    printf("Tensors:  %u\n", reader.n_tensors);
    printf("Tier:     %u (%s)\n\n", tier, pogls_addr_tier_name(tier));

    for (uint32_t i = 0; i < reader.n_tensors; i++) {
        uint32_t addr = pogls_addr_from_name(reader.names[i], tier);
        PoglsAddrDecomp d = pogls_addr_decompose(addr, tier);

        printf("[%4u] %-40s  addr=%5u  macro=%3u  micro=%3u  %s  %u bytes\n",
               i, reader.names[i], addr, d.macro, d.micro,
               gguf_type_name(0), reader.sizes[i]);
    }

    gguf_close(&reader);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *name = NULL;
    const char *file = NULL;
    uint8_t tier = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file = argv[++i];
        } else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc) {
            tier = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        }
    }

    if (name) {
        resolve_name(name, tier);
    } else if (file) {
        resolve_file(file, tier);
    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}
