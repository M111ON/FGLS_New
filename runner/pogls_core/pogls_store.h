#ifndef POGLS_STORE_H
#define POGLS_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — implementation owns the struct layout */
typedef struct PoglsStore PoglsStore;

/* ── Store lifecycle ── */

PoglsStore* pogls_store_open(const char *path, uint64_t capacity);
void        pogls_store_close(PoglsStore *store);
int         pogls_store_sync(PoglsStore *store, int async);

/* ── Put / Get / Delete ── */

int  pogls_store_put(PoglsStore *store, const char *name,
                     const void *data, size_t sz);

void* pogls_store_get(PoglsStore *store, const char *name, size_t *sz_out);

int  pogls_store_free(PoglsStore *store, const char *name);

/* ── Enumeration ── */

typedef int (*PoglsStoreVisitFn)(const char *name, size_t sz,
                                 void *user_data);

int  pogls_store_foreach(PoglsStore *store, PoglsStoreVisitFn fn,
                         void *user_data);

uint32_t pogls_store_count(PoglsStore *store);
uint64_t pogls_store_bytes(PoglsStore *store);

/* ─── Queries ── */

int  pogls_store_has(PoglsStore *store, const char *name);
size_t pogls_store_size(PoglsStore *store, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_STORE_H */
