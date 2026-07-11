/*
 * pogls_platform.h — Platform Abstraction Layer
 *
 * Isolates all OS-specific code into one file.
 * Business logic headers include this for memory/file operations.
 *
 * API:
 *   pogls_map_file()    — read-only mmap a file
 *   pogls_alloc_large() — allocate large contiguous memory (VirtualAlloc/mmap)
 *   pogls_free_large()  — free large memory
 *   pogls_fopen()       — fopen wrapper
 *   pogls_fseek()       — 64-bit fseek
 *   pogls_ftell()       — 64-bit ftell
 */

#ifndef POGLS_PLATFORM_H
#define POGLS_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

/* ── Memory Mapping ─────────────────────────────────────── */

#ifdef _WIN32

static inline void* pogls_map_file(const char *path, size_t *size_out) {
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;

    DWORD hi = 0;
    DWORD lo = GetFileSize(hf, &hi);
    size_t sz = ((uint64_t)hi << 32) | lo;
    if (size_out) *size_out = sz;

    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) { CloseHandle(hf); return NULL; }

    void *ptr = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hm);
    CloseHandle(hf);
    if (!ptr) return NULL;
    return ptr;
}

static inline void* pogls_alloc_large(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static inline void pogls_free_large(void *ptr, size_t size) {
    (void)size;
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

#else /* POSIX */

static inline void* pogls_map_file(const char *path, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t sz = (size_t)st.st_size;
    if (size_out) *size_out = sz;

    void *ptr = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return NULL;
    return ptr;
}

static inline void* pogls_alloc_large(size_t size) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return NULL;
    return ptr;
}

static inline void pogls_free_large(void *ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

#endif

/* ── File I/O ───────────────────────────────────────────── */

static inline FILE* pogls_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

#ifdef _WIN32

static inline int pogls_fseek(FILE *f, int64_t offset, int origin) {
    return _fseeki64(f, offset, origin);
}

static inline int64_t pogls_ftell(FILE *f) {
    return _ftelli64(f);
}

#else

static inline int pogls_fseek(FILE *f, int64_t offset, int origin) {
    return fseeko(f, offset, origin);
}

static inline int64_t pogls_ftell(FILE *f) {
    return ftello(f);
}

#endif

#endif /* POGLS_PLATFORM_H */
