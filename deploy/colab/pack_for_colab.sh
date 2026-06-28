#!/bin/bash
# pack_for_colab.sh — Create a clean Colab tarball with exactly the files needed
#   to build the POGLS runner with DRamTile + SID on Linux/Colab.
#
# Usage:
#   cd /path/to/FGLS_new
#   bash deploy/colab/pack_for_colab.sh
#
# Output: FGLS_new_colab.tar.gz  (upload this to Colab)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTDIR="/tmp/FGLS_new_colab"
OUTTAR="$(pwd)/FGLS_new_colab.tar.gz"

echo "=== Packing POGLS Runner for Colab ==="
echo "Root: $ROOT"
echo "Output: $OUTTAR"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# ── Helper: copy preserving relative path ──
cp_if() {
    local src="$ROOT/$1"
    local dst="$OUTDIR/$1"
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
}

# ── 1. Runner source files (27 files) ──
echo "[runner] copying source files..."
for f in \
    runner/llama_pogls_runner_sid_v2.c \
    runner/kv_tensor_access.cpp \
    runner/kv_tensor_access.h \
    runner/gguf_index.h \
    runner/sid_cache.h \
    runner/sid_loader.h \
    runner/sid_delta_ring.h \
    runner/sid_timetravel.h \
    runner/bond_discovery.h \
    runner/hex_grid.h \
    runner/th_grid.h \
    runner/goldberg_sid.h \
    runner/cosplay.h \
    runner/session_profile.h \
    runner/dramtile_store.h \
    runner/dramtile_container.h \
    runner/kv_sid_evict.h \
    runner/kv_page_store.h \
    runner/kv_swap.h \
    runner/kv_remap.h \
    runner/kv_remap_rail.h \
    runner/capture_pipeline.h \
    runner/geo_addr.h \
    runner/gear_lock.h; do
    cp_if "$f"
done

# ── 2. Collection headers (transitive deps, 30 files) ──
echo "[collection] copying headers..."
# root
for f in \
    collection/coord_spine.h \
    collection/geo_dram_tile.h \
    collection/geo_frame_seek.h \
    collection/geom_raw_bridge.h \
    collection/lc_tantrix.h \
    collection/shadow_zone.h \
    collection/tri_hex_tess.h \
    collection/tw_bridge.h \
    collection/tw_capture_int.h \
    collection/tw_face_bridge.h \
    collection/tw_tensor_capture.h \
    collection/zone_card_sid.h \
    collection/goldberg_sid.h \
    collection/hex_codec.h \
    collection/sid.h \
    collection/pogls_pipeline.h \
    collection/pogls_bond.h; do
    cp_if "$f"
done

# core
for f in \
    collection/core/core/pogls_platform.h \
    collection/core/core/pogls_fold.h; do
    cp_if "$f"
done

# geo_jump_module
for f in \
    collection/geo_jump_module/include/geo_jump.h \
    collection/geo_jump_module/include/geo_shell.h \
    collection/geo_jump_module/include/geo_shell_fold.h \
    collection/geo_jump_module/include/geo_dodeca_adj.h \
    collection/geo_jump_module/include/geo_dodeca_ring.h \
    collection/geo_jump_module/include/geo_field_ring.h \
    collection/geo_jump_module/include/geo_field_climate.h \
    collection/geo_jump_module/include/onion_shell.h \
    collection/geo_jump_module/include/onion_stack.h \
    collection/geo_jump_module/include/shell_container.h \
    collection/geo_jump_module/include/shell_hop.h \
    collection/geo_jump_module/include/shell_weight_map.h \
    collection/geo_jump_module/include/weight_bond_codec.h \
    collection/geo_jump_module/include/zone_card.h \
    collection/geo_jump_module/src/geo_jump.c; do
    cp_if "$f"
done

# src (collection/src)
for f in \
    collection/src/tensor_memory.h \
    collection/src/icosa_bridge_loader.h \
    collection/src/zone_card.h \
    collection/src/lc_twin_gate.h \
    collection/src/lc_hdr.h \
    collection/src/gear_lock.h \
    collection/src/tgw_cardioid_express.h \
    collection/src/tgw_dispatch.h \
    collection/src/tgw_dispatch_v2.h \
    collection/src/tgw_dispatch_v2_tri5.h \
    collection/src/tgw_frustum_wire.h \
    collection/src/tgw_ground_lcgw.h \
    collection/src/tgw_ground_lcgw_lazy.h \
    collection/src/tgw_lc_bridge.h \
    collection/src/tgw_lc_bridge_p6.h \
    collection/src/tgw_stream_dispatch.h \
    collection/src/tgw_tri5_wire.h \
    collection/src/icosa_twin_bridge.h \
    collection/src/frustum_trit.h \
    collection/src/frustum_slot64.h \
    collection/src/frustum_layout_v2.h \
    collection/src/frustum_gcfs.h \
    collection/src/fibo_spine.h \
    collection/src/fabric_wire.h \
    collection/src/fabric_wire_drain.h \
    collection/src/lc_wire.h \
    collection/src/lc_fs.h \
    collection/src/lc_delete.h \
    collection/src/lcgw_adaptive.h \
    collection/src/lc_hdr_lazy.h \
    collection/src/geo_temporal_lut.h \
    collection/src/geo_metatron_route.h \
    collection/src/geo_letter_cube.h \
    collection/src/geo_compound_cfg.h \
    collection/src/geo_goldberg_lut.h \
    collection/src/geo_goldberg_tile.h \
    collection/src/geo_pixel.h \
    collection/src/geo_addr_net.h \
    collection/src/geo_gpx_anim.h \
    collection/src/hex_tile.h \
    collection/src/heptagon_fence.h; do
    cp_if "$f"
done

# diamond/dgls
for f in \
    collection/dgls/diamond/include/diamond_shell_codec.h \
    collection/dgls/diamond/include/diamond_shell_v2.h \
    collection/dgls/diamond/include/pogls_fold.h \
    collection/dgls/diamond/include/binary_shell_codec.h; do
    if [ -f "$ROOT/$f" ]; then cp_if "$f"; fi
done

# Also check geopixel path for shell codec (older location)
for f in \
    collection/geopixel/binary_shell_codec.h \
    collection/geopixel/hbv_bundle/Diamond_decode_hamburger/diamond_shell_codec.h \
    collection/geopixel/hbv_bundle/Diamond_shell_encoder/diamond_shell_v2.h \
    collection/geopixel/hbv_bundle/Diamond_shell_encoder/pogls_fold.h; do
    if [ -f "$ROOT/$f" ]; then cp_if "$f"; fi
done

# ── 3. Build + run script ──
echo "[script] copying build_runner.sh..."
cp "$ROOT/deploy/colab/build_runner.sh" "$OUTDIR/build_runner.sh"
chmod +x "$OUTDIR/build_runner.sh"

# ── 4. Create tarball ──
echo ""
echo ">>> Creating tarball..."
cd /tmp
tar czf "$OUTTAR" FGLS_new_colab
ls -lh "$OUTTAR"
echo "=== DONE ==="
echo "Upload $OUTTAR to Colab, then:"
echo "  tar xzf FGLS_new_colab.tar.gz"
echo "  cd FGLS_new_colab"
echo "  bash build_runner.sh"
