/*
 * pipeline_store.h — Lightweight mmap-backed store for GeoField pipeline
 *
 * Self-contained: no dependencies on runner/ headers.
 * Provides: put(name, data, size) → pointer (zero-copy)
 *           get(name) → pointer (O(1))
 *           destroy → cleanup
 *
 * Backed by VirtualAlloc (Windows) or mmap (Linux).
 * Hash table with FNV-1a addressing. 512 slots.
 */

#ifndef PIPELINE_STORE_H
#define PIPELINE_STORE_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #endif
  #include <sys/mman.h>
  #include <unistd.h>
#endif

#define PS_HASH_SLOTS  512
#define PS_NAME_MAX     64

typedef struct {
    uint32_t addr;          /* hash-derived key (0 = unused) */
    size_t   offset;        /* byte offset in base */
    size_t   size;          /* stored bytes */
    char     name[PS_NAME_MAX];
} PSHashEntry;

typedef struct {
    uint8_t  *base;
    size_t    capacity;
    size_t    used;
    PSHashEntry hash[PS_HASH_SLOTS];
    uint32_t    n_stored;
} PipelineStore;

/* FNV-1a hash */
static inline uint32_t ps_hash_name(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1; /* avoid 0 (sentinel) */
}

/* Init store with capacity bytes */
static inline int ps_init(PipelineStore *s, size_t capacity) {
    memset(s, 0, sizeof(*s));
    s->capacity = capacity;

#ifdef _WIN32
    s->base = (uint8_t *)VirtualAlloc(NULL, capacity,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    s->base = (uint8_t *)mmap(NULL, capacity,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->base == MAP_FAILED) s->base = NULL;
#endif

    return s->base ? 0 : -1;
}

/* Destroy store */
static inline void ps_destroy(PipelineStore *s) {
    if (s->base) {
#ifdef _WIN32
        VirtualFree(s->base, 0, MEM_RELEASE);
#else
        munmap(s->base, s->capacity);
#endif
        s->base = NULL;
    }
}

/* Store data by name. Returns pointer into mmap (zero-copy). */
static inline uint8_t *ps_put(PipelineStore *s, const char *name,
                               const uint8_t *data, size_t sz) {
    uint32_t addr = ps_hash_name(name);
    uint32_t slot = addr % PS_HASH_SLOTS;

    /* Open addressing: linear probe on collision */
    for (uint32_t probe = 0; probe < PS_HASH_SLOTS; probe++) {
        uint32_t idx = (slot + probe) % PS_HASH_SLOTS;

        if (s->hash[idx].addr == addr && s->hash[idx].size > 0) {
            /* Same name found — overwrite if same size */
            if (s->hash[idx].size != sz) return NULL;
            memcpy(s->base + s->hash[idx].offset, data, sz);
            strncpy(s->hash[idx].name, name, PS_NAME_MAX - 1);
            return s->base + s->hash[idx].offset;
        }

        if (s->hash[idx].addr == 0) {
            /* Empty slot — store here */
            size_t off = (s->used + 63) & ~63;
            if (off + sz > s->capacity) return NULL;
            memcpy(s->base + off, data, sz);
            s->hash[idx].addr = addr;
            s->hash[idx].offset = off;
            s->hash[idx].size = sz;
            strncpy(s->hash[idx].name, name, PS_NAME_MAX - 1);
            s->hash[idx].name[PS_NAME_MAX - 1] = '\0';
            if (off + sz > s->used) s->used = off + sz;
            s->n_stored++;
            return s->base + off;
        }
    }

    return NULL; /* table full */
}

/* Get data pointer by name (O(1) amortized). Returns NULL if not found. */
static inline uint8_t *ps_get(PipelineStore *s, const char *name) {
    uint32_t addr = ps_hash_name(name);
    uint32_t slot = addr % PS_HASH_SLOTS;

    for (uint32_t probe = 0; probe < PS_HASH_SLOTS; probe++) {
        uint32_t idx = (slot + probe) % PS_HASH_SLOTS;

        if (s->hash[idx].addr == addr && s->hash[idx].size > 0) {
            return s->base + s->hash[idx].offset;
        }
        if (s->hash[idx].addr == 0) {
            return NULL; /* empty slot = not found */
        }
    }
    return NULL;
}

/* Get stored size by name. Returns 0 if not found. */
static inline size_t ps_get_size(PipelineStore *s, const char *name) {
    uint32_t addr = ps_hash_name(name);
    uint32_t slot = addr % PS_HASH_SLOTS;

    for (uint32_t probe = 0; probe < PS_HASH_SLOTS; probe++) {
        uint32_t idx = (slot + probe) % PS_HASH_SLOTS;
        if (s->hash[idx].addr == addr && s->hash[idx].size > 0) {
            return s->hash[idx].size;
        }
        if (s->hash[idx].addr == 0) return 0;
    }
    return 0;
}

/* Print stats */
static inline void ps_print_stats(const PipelineStore *s) {
    printf("PipelineStore: %u entries, %zu/%zu bytes (%.1f%% used)\n",
           s->n_stored, s->used, s->capacity,
           s->capacity > 0 ? 100.0 * s->used / s->capacity : 0);
}

#endif /* PIPELINE_STORE_H */
