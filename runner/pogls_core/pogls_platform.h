#ifndef POGLS_PLATFORM_H
#define POGLS_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

/* ── Memory Query ── */

#define POGLS_PAGE_NONE                0
#define POGLS_PAGE_READONLY            1
#define POGLS_PAGE_READWRITE           2
#define POGLS_PAGE_WRITECOPY           3
#define POGLS_PAGE_EXECUTE             4
#define POGLS_PAGE_EXECUTE_READ        5
#define POGLS_PAGE_EXECUTE_READWRITE   6
#define POGLS_PAGE_EXECUTE_WRITECOPY   7

#define POGLS_MEM_FREE      0
#define POGLS_MEM_COMMIT    1
#define POGLS_MEM_RESERVE   2

typedef struct {
    void    *base_addr;
    void    *alloc_base;
    size_t   region_size;
    uint32_t state;
    uint32_t protect;
} PoglsMemInfo;

static inline int pogls_query_memory(const void *addr, PoglsMemInfo *info) {
    if (!info) return -1;
    memset(info, 0, sizeof(*info));
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
        return -1;
    info->base_addr   = mbi.BaseAddress;
    info->alloc_base  = mbi.AllocationBase;
    info->region_size = mbi.RegionSize;
    if (mbi.State & MEM_COMMIT)   info->state = POGLS_MEM_COMMIT;
    else if (mbi.State & MEM_RESERVE) info->state = POGLS_MEM_RESERVE;
    else info->state = POGLS_MEM_FREE;
    if (mbi.Protect & (PAGE_READWRITE|PAGE_WRITECOPY))  info->protect = POGLS_PAGE_READWRITE;
    else if (mbi.Protect & PAGE_READONLY)                info->protect = POGLS_PAGE_READONLY;
    else if (mbi.Protect & (PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) info->protect = POGLS_PAGE_EXECUTE_READWRITE;
    else if (mbi.Protect & PAGE_EXECUTE_READ) info->protect = POGLS_PAGE_EXECUTE_READ;
    else if (mbi.Protect & PAGE_EXECUTE)      info->protect = POGLS_PAGE_EXECUTE;
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (page == 0) page = 4096;
    const char *p = (const char *)addr;
    const char *base = (const char *)((uintptr_t)p & ~(page - 1));
    info->base_addr = (void *)base;
    info->alloc_base = info->base_addr;
    info->region_size = page;
    info->state = POGLS_MEM_COMMIT;
    info->protect = POGLS_PAGE_READWRITE;
#endif
    return 0;
}

static inline int pogls_module_dir(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return -1;
#ifdef _WIN32
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
    if (r == 0 || r >= buf_size) return -1;
    char *sep = strrchr(buf, '\\');
    if (sep) sep[1] = 0; else buf[0] = 0;
    return 0;
#else
    ssize_t r = readlink("/proc/self/exe", buf, buf_size - 1);
    if (r <= 0) return -1;
    buf[r] = 0;
    char *sep = strrchr(buf, '/');
    if (sep) sep[1] = 0; else buf[0] = 0;
    return 0;
#endif
}

static inline int pogls_is_valid_ptr(const void *addr) {
    if (!addr || (uintptr_t)addr < 0x10000) return 0;
    PoglsMemInfo info;
    if (pogls_query_memory(addr, &info) != 0) return 0;
    return info.state == POGLS_MEM_COMMIT
        && (info.protect == POGLS_PAGE_READONLY
         || info.protect == POGLS_PAGE_READWRITE
         || info.protect == POGLS_PAGE_EXECUTE_READ
         || info.protect == POGLS_PAGE_EXECUTE_READWRITE);
}

#ifdef __cplusplus
}
#endif

#endif /* POGLS_PLATFORM_H */
