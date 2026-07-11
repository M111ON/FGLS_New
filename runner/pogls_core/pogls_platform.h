#ifndef POGLS_PLATFORM_H
#define POGLS_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Memory Mapping ── */

void* pogls_map_file(const char *path, size_t *size_out);

void* pogls_alloc_large(size_t size);

void  pogls_free_large(void *ptr, size_t size);

/* ── File I/O ── */

FILE* pogls_fopen(const char *path, const char *mode);

int   pogls_fseek(FILE *f, int64_t offset, int origin);

int64_t pogls_ftell(FILE *f);

int64_t pogls_fsize(const char *path);

/* ── Utilities ── */

uint32_t pogls_xorshift32(uint32_t *state);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_PLATFORM_H */
