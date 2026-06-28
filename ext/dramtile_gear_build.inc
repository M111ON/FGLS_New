# ══════════════════════════════════════════════════════════════════
# dramtile_gear_build.inc — Build configuration for DRamTile Gear
# ══════════════════════════════════════════════════════════════════
# Include in your Makefile / build script:
#   include ext/dramtile_gear_build.inc
#   $(CC) $(DRAMTILE_GEAR_CFLAGS) -c myfile.c ...
# ══════════════════════════════════════════════════════════════════

# ── Compiler flags ────────────────────────────────────────────────
# These -I paths resolve every include in the full dependency chain:
#
#   ext/dramtile_gear.h
#     → lc_twin_gate.h          (real: collection/src/)
#     → geo_giant_array.h       (collection/core/core/)
#     → geo_cube_file_store.h   (collection/core/core/)
#     → twin_gear_bridge.h      (collection/src/)
#       → pogls_twin_bridge.h   (collection/core/pogls_engine/)
#         → core/geo_config.h   (collection/core/pogls_engine/core/)
#         → twin_core/*.h       (collection/core/pogls_engine/twin_core/)
#       → icosa_twin_bridge.h   (collection/src/)
#       → gear_lock.h           (collection/src/)
#     → runner/dramtile_store.h (runner/)
#       → geo_dram_tile.h       (collection/)
#
#   Transitive:
#     → core/geo_pipeline_wire.h   (engine/core/)
#     → core/pogls_geomatrix.h     (engine/core/)
#     → core/pogls_qrpn_phaseE.h   (engine/core/)
#     → twin_core/geo_dodeca_torus, geo_hardening_whe, theta_map,
#       geo_route, geo_whe, geo_read, pogls_fold, geo_thirdeye  (twin_core/)
#     → collection/core/core/geo_letter_cube, geo_apex_wire,
#       geo_primitives, pogls_fibo_addr, geo_config               (core/core/)
#     → collection/coord_spine.h   (collection/)
# ══════════════════════════════════════════════════════════════════

# CRITICAL: twin_core must come BEFORE core/core to resolve
# geo_fibo_clock.h from the POGLS engine directory, not from core/core/.
# The -I order determines which file wins when filenames collide.
DRAMTILE_GEAR_CFLAGS := \
    -std=c11 \
    -I. \
    -Icollection \
    -Icollection/src \
    -Icollection/core/pogls_engine/twin_core \
    -Icollection/core/pogls_engine \
    -Icollection/core/pogls_engine/core \
    -Icollection/core/core

# ── Link flags ────────────────────────────────────────────────────
# stdc++ needed for C++ linker driver (ld/gcc -m64) when linking
# with libllama / libggml.  -lm for math (geo route / hilbert).
DRAMTILE_GEAR_LIBS := -lm

# ── Usage example (from repo root) ─────────────────────────────────
# gcc $(DRAMTILE_GEAR_CFLAGS) -fsyntax-only ext/dramtile_gear.h
#
# Full compile + link example:
#   gcc $(DRAMTILE_GEAR_CFLAGS) -O2 -c -o myfile.o myfile.c
#   gcc -m64 -o myprogram.exe myfile.o $(DRAMTILE_GEAR_LIBS) -L. -llibllama -lggml -lstdc++
