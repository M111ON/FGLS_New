/*
 * pogls_bond_export.h
 * ─────────────────────────────────────────────────────────────
 * Shared-library export shim for pogls_bond.h
 *
 * Exposes C bond operations as plain C ABI symbols callable from
 * Python ctypes / cffi — no subprocess, no serialization overhead.
 *
 * Compile to .so / .dll:
 *   Linux:   gcc -O2 -shared -fPIC -I. -o pogls_bond.so pogls_bond_export.c
 *   macOS:   gcc -O2 -dynamiclib -I. -o pogls_bond.dylib pogls_bond_export.c
 *   Windows: gcc -O2 -shared -I. -o pogls_bond.dll pogls_bond_export.c
 *
 * All structs use fixed-width types and packed layout — safe for
 * ctypes Structure mapping without alignment surprises.
 * ─────────────────────────────────────────────────────────────
 */

#ifndef POGLS_BOND_EXPORT_H
#define POGLS_BOND_EXPORT_H

#include <stdint.h>
#include "pogls_bond.h"

#ifdef _WIN32
#  define POGLS_API __declspec(dllexport)
#else
#  define POGLS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Raw hash ────────────────────────────────────────────── */
POGLS_API uint64_t  poglsex_fibo_addr     (uint64_t seed);
POGLS_API uint64_t  poglsex_seed_from_fp  (const char *topology_fp);

/* ── Piece factory ───────────────────────────────────────── */

/*
 * Fills caller-allocated PoglsPiece.
 * Python side creates ctypes.Structure of 25 bytes and passes pointer.
 */
POGLS_API void      poglsex_make_piece    (uint64_t origin_seed,
                                           uint8_t  fold_axis,
                                           PoglsPiece *out);

/* ── Bond operations ─────────────────────────────────────── */
POGLS_API uint64_t  poglsex_bond_key      (const PoglsPiece *p);

/*
 * Returns 1 if valid, 0 if not.
 * bond_key_out (nullable): receives raw XOR (stable cross-session).
 */
POGLS_API uint8_t   poglsex_bond_verify   (const PoglsPiece *a,
                                           const PoglsPiece *b,
                                           uint64_t *bond_key_out);

/* ── Slot operations ─────────────────────────────────────── */
POGLS_API void      poglsex_make_slot     (uint64_t origin_seed,
                                           uint8_t  fold_axis,
                                           uint32_t agent_id,
                                           uint32_t token_cap,
                                           PoglsSlot *out);

POGLS_API void      poglsex_plug_connect  (PoglsSlot *a, uint8_t face_a,
                                           PoglsSlot *b, uint8_t face_b,
                                           uint16_t ttl);

POGLS_API void      poglsex_plug_disconnect(PoglsSlot *slot, uint8_t face);

POGLS_API void      poglsex_reroute       (PoglsSlot *slot, uint8_t fault);

/* ── Session nonce ───────────────────────────────────────── */
POGLS_API void      poglsex_set_nonce     (uint64_t nonce);
POGLS_API uint64_t  poglsex_get_nonce     (void);

/* ── Version / config ────────────────────────────────────── */
POGLS_API const char *poglsex_version     (void);
POGLS_API int         poglsex_verify_bits (void);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_BOND_EXPORT_H */
