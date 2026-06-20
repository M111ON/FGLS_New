/*
 * pogls_platform.h — POGLS V3.9  Cross-Platform Compatibility
 * ══════════════════════════════════════════════════════════════════════
 *
 * Handles:
 *   fsync()       Linux/Mac → FlushFileBuffers() Windows
 *   mmap()        Linux/Mac → CreateFileMapping() Windows
 *   atomic ops    GCC built-ins → MSVC interlocked
 *   threading     pthread → Win32 threads
 *
 * Usage: #include "pogls_platform.h" ก่อน header อื่นทุกตัว
 * ══════════════════════════════════════════════════════════════════════
 */
#ifndef POGLS_PLATFORM_H
#define POGLS_PLATFORM_H

/* ── Detect Platform ─────────────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
  #define POGLS_WINDOWS 1
#elif defined(__linux__)
  #define POGLS_LINUX   1
#elif defined(__APPLE__)
  #define POGLS_MACOS   1
#endif

/* ══════════════════════════════════════════════════════════════════
 * Windows Compatibility Layer
 * ══════════════════════════════════════════════════════════════════ */
#ifdef POGLS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <stdint.h>
#include <stdio.h>

/* fsync → FlushFileBuffers */
static inline int pogls_fsync(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}
#define fsync(fd)  pogls_fsync(fd)

/* mkdir → _mkdir (Windows single arg) */
#define mkdir(path, mode)  _mkdir(path)

/* usleep → Sleep (ms) */
#define usleep(us)  Sleep((us) / 1000)

/* ssize_t */
typedef long long ssize_t;

/* mmap constants (stub — use pogls_mmap instead) */
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define MAP_SHARED   0x1
#define MAP_FAILED   ((void*)-1)

/* Windows mmap via CreateFileMapping */
#include <sys/types.h>

static inline void *pogls_mmap(void *addr, size_t length,
                                int prot, int flags,
                                int fd, off_t offset)
{
    (void)addr; (void)flags; (void)offset;
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return MAP_FAILED;

    DWORD protect = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE hMap   = CreateFileMapping(hFile, NULL, protect, 0, 0, NULL);
    if (!hMap) return MAP_FAILED;

    DWORD access  = (prot & PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    void *ptr     = MapViewOfFile(hMap, access, 0, 0, length);
    CloseHandle(hMap);
    return ptr ? ptr : MAP_FAILED;
}
#define mmap(a,l,p,f,fd,o)  pogls_mmap(a,l,p,f,fd,o)

static inline int pogls_munmap(void *addr, size_t length)
{
    (void)length;
    return UnmapViewOfFile(addr) ? 0 : -1;
}
#define munmap(a,l)  pogls_munmap(a,l)

/* madvise stub (no-op on Windows) */
#define MADV_DONTNEED  0
#define MADV_WILLNEED  0
static inline int madvise(void *a, size_t l, int advice)
{ (void)a;(void)l;(void)advice; return 0; }

/* pthread → Win32 threads minimal shim */
#ifndef _PTHREAD_H
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

#define pthread_mutex_init(m,a)    InitializeCriticalSection(m)
#define pthread_mutex_lock(m)      EnterCriticalSection(m)
#define pthread_mutex_unlock(m)    LeaveCriticalSection(m)
#define pthread_mutex_destroy(m)   DeleteCriticalSection(m)

typedef struct { HANDLE h; void*(*fn)(void*); void*arg; } _PthreadCtx;
static DWORD WINAPI _pthread_wrapper(LPVOID p) {
    _PthreadCtx *c = (_PthreadCtx*)p;
    c->fn(c->arg); return 0;
}
static inline int pthread_create(pthread_t *t, void *attr,
                                  void*(*fn)(void*), void *arg) {
    (void)attr;
    _PthreadCtx *c = (_PthreadCtx*)malloc(sizeof(*c));
    c->fn=fn; c->arg=arg;
    *t = CreateThread(NULL,0,_pthread_wrapper,c,0,NULL);
    return *t ? 0 : -1;
}
static inline int pthread_join(pthread_t t, void **r) {
    (void)r; WaitForSingleObject(t,INFINITE); CloseHandle(t); return 0;
}
#endif /* _PTHREAD_H */

/* atomic — use GCC built-ins if MinGW, else MSVC interlocked */
#if !defined(__GNUC__)
  #define __sync_fetch_and_add(p,v)  InterlockedAdd64((LONG64*)(p),(v))
  #define __builtin_expect(x,y)      (x)
  #define __builtin_prefetch(p,r,l)  (void)(p)
#endif

/* ══════════════════════════════════════════════════════════════════
 * Linux / macOS — standard headers
 * ══════════════════════════════════════════════════════════════════ */
#else /* Linux / macOS */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

/* pogls_fsync = standard fsync */
#define pogls_fsync(fd)  fsync(fd)

#endif /* platform */

/* ══════════════════════════════════════════════════════════════════
 * Common across all platforms
 * ══════════════════════════════════════════════════════════════════ */

/* compiler hints */
#ifndef likely
  #define likely(x)    __builtin_expect(!!(x), 1)
  #define unlikely(x)  __builtin_expect(!!(x), 0)
#endif

/* cache line size */
#ifndef POGLS_CACHE_LINE
  #define POGLS_CACHE_LINE  64u
#endif

/* align helper */
#ifdef POGLS_WINDOWS
  #define POGLS_ALIGN(n)  __declspec(align(n))
#else
  #define POGLS_ALIGN(n)  __attribute__((aligned(n)))
#endif

/* packed struct */
#ifdef POGLS_WINDOWS
  #define POGLS_PACKED  __pragma(pack(push,1)) 
  #define POGLS_PACKED_END __pragma(pack(pop))
#else
  #define POGLS_PACKED      /* use __attribute__((packed)) per struct */
  #define POGLS_PACKED_END
#endif

/* path separator */
#ifdef POGLS_WINDOWS
  #define POGLS_PATH_SEP  "\\"
#else
  #define POGLS_PATH_SEP  "/"
#endif

/* snprintf path helper */
#define POGLS_PATH(buf, dir, file) \
    snprintf(buf, sizeof(buf), "%s" POGLS_PATH_SEP "%s", dir, file)

/* version */
#define POGLS_VERSION_MAJOR  3
#define POGLS_VERSION_MINOR  9
#define POGLS_VERSION_PATCH  2
#define POGLS_VERSION_STR    "3.9.2"

/* ══════════════════════════════════════════════════════════════════
 * Memory Query — cross-platform VirtualQuery replacement
 * ══════════════════════════════════════════════════════════════════ */

/* Protection flags (unified) */
#define POGLS_PAGE_NONE                0
#define POGLS_PAGE_READONLY            1
#define POGLS_PAGE_READWRITE           2
#define POGLS_PAGE_WRITECOPY           3
#define POGLS_PAGE_EXECUTE             4
#define POGLS_PAGE_EXECUTE_READ        5
#define POGLS_PAGE_EXECUTE_READWRITE   6
#define POGLS_PAGE_EXECUTE_WRITECOPY   7

/* Memory state flags */
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

#ifdef POGLS_WINDOWS
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
        return -1;
    info->base_addr   = mbi.BaseAddress;
    info->alloc_base  = mbi.AllocationBase;
    info->region_size = mbi.RegionSize;
    info->state = (mbi.State == MEM_COMMIT)   ? POGLS_MEM_COMMIT
                : (mbi.State == MEM_RESERVE)   ? POGLS_MEM_RESERVE
                :                                POGLS_MEM_FREE;
    if (mbi.Protect & PAGE_READONLY)            info->protect = POGLS_PAGE_READONLY;
    if (mbi.Protect & PAGE_READWRITE)           info->protect = POGLS_PAGE_READWRITE;
    if (mbi.Protect & PAGE_WRITECOPY)           info->protect = POGLS_PAGE_WRITECOPY;
    if (mbi.Protect & PAGE_EXECUTE)             info->protect = POGLS_PAGE_EXECUTE;
    if (mbi.Protect & PAGE_EXECUTE_READ)        info->protect = POGLS_PAGE_EXECUTE_READ;
    if (mbi.Protect & PAGE_EXECUTE_READWRITE)   info->protect = POGLS_PAGE_EXECUTE_READWRITE;
    if (mbi.Protect & PAGE_EXECUTE_WRITECOPY)   info->protect = POGLS_PAGE_EXECUTE_WRITECOPY;
    return 0;

#else
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end;
        char perm[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perm) < 3) continue;
        uintptr_t a = (uintptr_t)addr;
        if (a >= start && a < end) {
            info->base_addr   = (void *)start;
            info->alloc_base  = (void *)start;
            info->region_size = (size_t)(end - start);
            info->state       = POGLS_MEM_COMMIT;
            if (perm[0] == 'r' && perm[1] == 'w' && perm[2] == 'x')
                info->protect = POGLS_PAGE_EXECUTE_READWRITE;
            else if (perm[1] == 'w' && perm[2] == 'x')
                info->protect = POGLS_PAGE_EXECUTE_READWRITE;
            else if (perm[0] == 'r' && perm[2] == 'x')
                info->protect = POGLS_PAGE_EXECUTE_READ;
            else if (perm[2] == 'x')
                info->protect = POGLS_PAGE_EXECUTE;
            else if (perm[0] == 'r' && perm[1] == 'w')
                info->protect = POGLS_PAGE_READWRITE;
            else if (perm[0] == 'r')
                info->protect = POGLS_PAGE_READONLY;
            else
                info->protect = POGLS_PAGE_NONE;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
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

static inline int pogls_module_dir(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return -1;
#ifdef POGLS_WINDOWS
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

#endif /* POGLS_PLATFORM_H */

/* ══════════════════════════════════════════════════════════════════
 * PHI ADDRESSING CONSTANTS (SINGLE SOURCE — FROZEN)
 * All subsystems MUST reference these via pogls_platform.h
 * Never redefine elsewhere.
 * ══════════════════════════════════════════════════════════════════ */
#ifndef POGLS_PHI_CONSTANTS
#define POGLS_PHI_CONSTANTS
#  define POGLS_PHI_SCALE   (1u  << 20)    /* 2^20 = 1,048,576         */
#  define POGLS_PHI_UP      1696631u        /* floor(phi  x 2^20)       */
#  define POGLS_PHI_DOWN     648055u        /* floor(phi^-1 x 2^20)     */
#  define POGLS_PHI_COMP     400521u        /* 2^20 - PHI_DOWN (wrap)   */
#endif
