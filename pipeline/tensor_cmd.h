/* tensor_cmd.h — Unified Tensor Field Command Interface
 * ═══════════════════════════════════════════════════════════════
 *
 * Integrates SID capture, addressing, routing, remap, GGUF scan
 * into a single clean interface for fgls_cli.c.
 *
 * Usage (via fgls.exe):
 *   fgls tensor test          → run full field verification (28 tests)
 *   fgls tensor addr <name>   → address decomposition
 *   fgls tensor route <name>  → show routing options
 *   fgls tensor capture <file> [out]  → SID capture
 *   fgls tensor summon <file> [out]   → reconstruct
 *   fgls tensor remap <file>          → capo rotation
 *   fgls tensor gguf <path>           → scan GGUF tensor map
 *
 * Separate TU (own static globals for test counters).
 * Compile with -DGEO_JUMP_INLINE.
 */

#ifndef TENSOR_CMD_H
#define TENSOR_CMD_H

#include <stdint.h>

/* ── Command dispatch ── */
int tensor_cmd_test(void);
int tensor_cmd_addr(const char *name);
int tensor_cmd_route(const char *name);
int tensor_cmd_capture(const char *in_path, const char *out_path);
int tensor_cmd_summon(const char *in_path, const char *out_path);
int tensor_cmd_remap(const char *in_path);
int tensor_cmd_gguf(const char *gguf_path);
int tensor_cmd_zero(const char *gguf_path);

#endif /* TENSOR_CMD_H */
