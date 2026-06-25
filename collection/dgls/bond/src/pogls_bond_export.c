/*
 * pogls_bond_export.c
 * ─────────────────────────────────────────────────────────────
 * Implementation of shared-library exports.
 * Thin wrappers — all logic lives in pogls_bond.h (inline).
 * ─────────────────────────────────────────────────────────────
 */

#include "pogls_bond_export.h"
#include <string.h>

POGLS_API uint64_t poglsex_fibo_addr(uint64_t seed) {
    return pogls_fibo_addr(seed);
}

POGLS_API uint64_t poglsex_seed_from_fp(const char *topology_fp) {
    return pogls_seed_from_fp(topology_fp);
}

POGLS_API void poglsex_make_piece(uint64_t origin_seed, uint8_t fold_axis,
                                   PoglsPiece *out) {
    if (!out) return;
    *out = pogls_make_piece(origin_seed, fold_axis);
}

POGLS_API uint64_t poglsex_bond_key(const PoglsPiece *p) {
    if (!p) return 0;
    return pogls_bond_key(p);
}

POGLS_API uint8_t poglsex_bond_verify(const PoglsPiece *a, const PoglsPiece *b,
                                       uint64_t *bond_key_out) {
    if (!a || !b) return 0;
    PoglsBond bond = pogls_bond_verify(a, b);
    if (bond_key_out) *bond_key_out = bond.bond_key;
    return bond.valid;
}

POGLS_API void poglsex_make_slot(uint64_t origin_seed, uint8_t fold_axis,
                                  uint32_t agent_id, uint32_t token_cap,
                                  PoglsSlot *out) {
    if (!out) return;
    memset(out, 0, sizeof(PoglsSlot));
    out->piece     = pogls_make_piece(origin_seed, fold_axis);
    out->agent_id  = agent_id;
    out->token_cap = token_cap;
    out->rerouted  = 0;
}

POGLS_API void poglsex_plug_connect(PoglsSlot *a, uint8_t face_a,
                                     PoglsSlot *b, uint8_t face_b,
                                     uint16_t ttl) {
    if (!a || !b) return;
    pogls_plug_connect(a, face_a, b, face_b, ttl);
}

POGLS_API void poglsex_plug_disconnect(PoglsSlot *slot, uint8_t face) {
    if (!slot) return;
    pogls_plug_disconnect(slot, face);
}

POGLS_API void poglsex_reroute(PoglsSlot *slot, uint8_t fault) {
    if (!slot) return;
    pogls_reroute(slot, (PoglsFault)fault);
}

POGLS_API void poglsex_set_nonce(uint64_t nonce) {
    pogls_config_set_nonce(nonce);
}

POGLS_API uint64_t poglsex_get_nonce(void) {
    return pogls_config_get_nonce();
}

POGLS_API const char *poglsex_version(void) {
    return POGLS_BOND_VERSION_STR;
}

POGLS_API int poglsex_verify_bits(void) {
    return POGLS_BOND_VERIFY_BITS;
}
