#include "pogls_platform.h"

void* pogls_map_file(const char *path, size_t *size_out) {
    if (!path || !size_out) return NULL;
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return NULL; }
    *size_out = (size_t)li.QuadPart;
    if (*size_out == 0) { CloseHandle(hFile); return NULL; }
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);
    if (!hMap) return NULL;
    void *ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, *size_out);
    CloseHandle(hMap);
    return ptr;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    *size_out = (size_t)st.st_size;
    if (*size_out == 0) { close(fd); return NULL; }
    void *ptr = mmap(NULL, *size_out, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return NULL;
    return ptr;
#endif
}

void* pogls_alloc_large(size_t size) {
    if (size == 0) return NULL;
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? NULL : ptr;
#endif
}

void pogls_free_large(void *ptr, size_t size) {
    if (!ptr) return;
    (void)size;
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

FILE* pogls_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

int pogls_fseek(FILE *f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, offset, origin);
#endif
}

int64_t pogls_ftell(FILE *f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

int64_t pogls_fsize(const char *path) {
    FILE *f = pogls_fopen(path, "rb");
    if (!f) return -1;
    if (pogls_fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t sz = pogls_ftell(f);
    fclose(f);
    return sz;
}

uint32_t pogls_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
