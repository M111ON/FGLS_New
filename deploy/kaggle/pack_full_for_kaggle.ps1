# pack_full_for_kaggle.ps1 — Pack ALL source + llama.cpp for Kaggle (no internet needed)
# ใช้: pwsh deploy/kaggle/pack_full_for_kaggle.ps1
#
# Output: deploy/kaggle/kaggle_full_pack.zip (~5 MB)
# อัปโหลด zip นี้ + model.framestore ไปที่ Kaggle Dataset
#
# วิธีใช้:
#   !cp -r /kaggle/input/YOUR-DATASET/kaggle_full_pack/* /kaggle/working/
#   !cp /kaggle/input/YOUR-DATASET/model.framestore /kaggle/working/
#   !bash /kaggle/working/build_kaggle.sh

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OUT = Join-Path $PSScriptRoot "kaggle_full_pack.zip"
$TMP = Join-Path $env:TMP "kaggle_full_$(Get-Random)"

Write-Host "=== Packing FGLS runner + llama.cpp for Kaggle ==="
New-Item -ItemType Directory -Path "$TMP/runner" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/src" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/core/core" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/rdh" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/geopixel" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/dgls/diamond/include" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/geo_jump_module/include" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/collection/geo_jump_module/src" -Force | Out-Null
New-Item -ItemType Directory -Path "$TMP/llama.cpp" -Force | Out-Null

# ── 1. ═══ Runner headers + source ═══
Write-Host "[1/3] Runner files..."
$runnerFiles = Get-ChildItem "$ROOT/runner" -Filter "*.h" -Name
$runnerFiles += Get-ChildItem "$ROOT/runner" -Filter "*.c" -Name
$runnerFiles += Get-ChildItem "$ROOT/runner" -Filter "*.cpp" -Name
foreach ($f in $runnerFiles) {
    Copy-Item "$ROOT/runner/$f" "$TMP/runner/$f"
}

# ── 2. ═══ Collection headers ═══
Write-Host "[2/3] Collection headers..."
# root collection
$colRoot = @(
    "coord_spine.h","geo_dram_tile.h","geo_frame_seek.h","geom_raw_bridge.h",
    "lc_tantrix.h","shadow_zone.h","tri_hex_tess.h",
    "tw_bridge.h","tw_capture_int.h","tw_face_bridge.h","tw_tensor_capture.h",
    "zone_card_sid.h","goldberg_sid.h","hex_codec.h","sid.h",
    "pogls_pipeline.h","pogls_bond.h"
)
foreach ($f in $colRoot) {
    $src = "$ROOT/collection/$f"
    if (Test-Path $src) { Copy-Item $src "$TMP/collection/$f" }
}

# collection/core/core
$colCore = @("pogls_platform.h","pogls_fold.h")
foreach ($f in $colCore) {
    Copy-Item "$ROOT/collection/core/core/$f" "$TMP/collection/core/core/$f"
}

# collection/src (31 files)
$colSrc = @(
    "tensor_memory.h","icosa_bridge_loader.h","zone_card.h",
    "lc_twin_gate.h","lc_hdr.h","gear_lock.h",
    "tgw_cardioid_express.h","tgw_dispatch.h","tgw_dispatch_v2.h","tgw_dispatch_v2_tri5.h",
    "tgw_frustum_wire.h","tgw_ground_lcgw.h","tgw_ground_lcgw_lazy.h",
    "tgw_lc_bridge.h","tgw_lc_bridge_p6.h","tgw_stream_dispatch.h","tgw_tri5_wire.h",
    "icosa_twin_bridge.h",
    "frustum_trit.h","frustum_slot64.h","frustum_layout_v2.h","frustum_gcfs.h",
    "fibo_spine.h","fabric_wire.h","fabric_wire_drain.h",
    "lc_wire.h","lc_fs.h","lc_delete.h","lcgw_adaptive.h","lc_hdr_lazy.h",
    "geo_temporal_lut.h","geo_metatron_route.h","geo_letter_cube.h",
    "geo_compound_cfg.h","geo_goldberg_lut.h","geo_goldberg_tile.h","geo_pixel.h",
    "geo_addr_net.h","geo_gpx_anim.h","hex_tile.h","heptagon_fence.h"
)
foreach ($f in $colSrc) {
    $src = "$ROOT/collection/src/$f"
    if (Test-Path $src) { Copy-Item $src "$TMP/collection/src/$f" }
}

# geopixel
@("binary_shell_codec.h") | ForEach-Object {
    $src = "$ROOT/collection/geopixel/$_"
    if (Test-Path $src) { Copy-Item $src "$TMP/collection/geopixel/$_" }
}

# dgls
@("diamond_shell_codec.h","diamond_shell_v2.h","pogls_fold.h","binary_shell_codec.h") | ForEach-Object {
    $src = "$ROOT/collection/dgls/diamond/include/$_"
    if (Test-Path $src) { Copy-Item $src "$TMP/collection/dgls/diamond/include/$_" }
}

# geo_jump_module
$geoJumpInclude = @(
    "geo_jump.h","geo_shell.h","geo_shell_fold.h",
    "geo_dodeca_adj.h","geo_dodeca_ring.h","geo_field_ring.h","geo_field_climate.h",
    "onion_shell.h","onion_stack.h","shell_container.h","shell_hop.h",
    "shell_weight_map.h","weight_bond_codec.h","zone_card.h"
)
foreach ($f in $geoJumpInclude) {
    $src = "$ROOT/collection/geo_jump_module/include/$f"
    if (Test-Path $src) { Copy-Item $src "$TMP/collection/geo_jump_module/include/$f" }
}
Copy-Item "$ROOT/collection/geo_jump_module/src/geo_jump.c" "$TMP/collection/geo_jump_module/src/geo_jump.c"

# rdh
Copy-Item "$ROOT/collection/rdh/rdh_addr.h" "$TMP/collection/rdh/rdh_addr.h"

# ── 3. ═══ llama.cpp source ═══
Write-Host "[3/3] llama.cpp source..."
$LLAMA = "I:/llama.cpp"
$llamaSubDirs = @("ggml","ggml/include","ggml/src","src","include","common")
foreach ($d in $llamaSubDirs) { New-Item -ItemType Directory -Path "$TMP/llama.cpp/$d" -Force | Out-Null }

$llamaFiles = @(
    "CMakeLists.txt",
    "ggml/CMakeLists.txt","ggml/src/CMakeLists.txt",
    "ggml/include/ggml.h","ggml/include/ggml-cuda.h",
    "ggml/include/ggml-alloc.h","ggml/include/ggml-backend.h",
    "ggml/src/ggml.c","ggml/src/ggml-alloc.c","ggml/src/ggml-backend.cpp",
    "ggml/src/ggml-cuda.cu","ggml/src/ggml-cuda.h",
    "ggml/src/ggml-quants.c","ggml/src/ggml-quants.h",
    "src/llama.cpp","src/llama.h",
    "src/llama-cuda.cpp","src/llama-cuda.h",
    "src/llama-graph.cpp","src/llama-graph.h",
    "src/llama-model.cpp","src/llama-model.h",
    "src/llama-model-loader.cpp","src/llama-model-loader.h",
    "src/llama-vocab.cpp","src/llama-vocab.h",
    "src/llama-grammar.cpp","src/llama-grammar.h",
    "src/llama-sampling.cpp","src/llama-sampling.h",
    "src/llama-batch.h","src/llama-chat.cpp","src/llama-chat.h",
    "src/llama-context.cpp","src/llama-hparams.h",
    "src/llama-memory.cpp","src/llama-memory.h",
    "src/llama-kv-cache.cpp","src/llama-kv-cache.h",
    "src/llama-mtypes.h","src/llama-arch.cpp","src/llama-arch.h",
    "src/llama-post-process.cpp",
    "include/llama.h","common/common.h","common/common.cpp"
)
foreach ($f in $llamaFiles) {
    $src = "$LLAMA/$f"
    if (Test-Path $src) { Copy-Item $src "$TMP/llama.cpp/$f" }
}

# ── 4. ═══ Build script ═══
Write-Host "[4/4] Build script..."
$bs = @'
#!/bin/bash
set -euo pipefail
cd /kaggle/working

echo "=== 1/3 Build llama.cpp (CUDA) ==="
cd llama.cpp && mkdir -p build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DLLAMA_CUDA=ON -DLLAMA_CUDA_F16=ON \
    -DLLAMA_VULKAN=OFF -DLLAMA_METAL=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF
make -j$(nproc) llama 2>&1 | tail -3
echo "OK"

echo "=== 2/3 Build Runner ==="
cd /kaggle/working
INCS="-I. -Irunner -Icollection -Icollection/src -Icollection/rdh \
      -Icollection/core/core -Icollection/geopixel \
      -Icollection/geo_jump_module/include \
      -Icollection/dgls/diamond/include \
      -Illama.cpp -Illama.cpp/ggml/include -Illama.cpp/src"
mkdir -p build

# geo_jump.c dependency
gcc -std=c11 -O2 $INCS -c collection/geo_jump_module/src/geo_jump.c -o build/geo_jump.o

# runner C
gcc -std=c11 -O2 $INCS -c runner/llama_pogls_runner_sid_v2.c -o build/runner.o

# runner C++
g++ -std=c++17 -O2 $INCS -c runner/kv_tensor_access.cpp -o build/kv_tensor_access.o

# link
g++ build/runner.o build/kv_tensor_access.o build/geo_jump.o \
    -Lllama.cpp/build/src -lllama \
    -Lllama.cpp/build/ggml/src -lggml -lggml-base -lggml-cpu \
    -lcuda -lcudart -lm -lpthread -ldl -lstdc++ \
    -Wl,-rpath,llama.cpp/build/src -Wl,-rpath,llama.cpp/build/ggml/src \
    -o build/pogls_runner
echo "OK ($(ls -lh build/pogls_runner | awk '{print $5}'))"

echo "=== 3/3 Inference ==="
echo ""
LD_LIBRARY_PATH="llama.cpp/build/src:llama.cpp/build/ggml/src" \
./build/pogls_runner ./model.framestore \
    --pogls-fs ./model.framestore \
    --sid-disable --ngl 99 \
    --prompt "Hello" --max-new 32 --temp 0 2>&1 | tail -20

echo "=== DONE ==="
'@
Set-Content -Path "$TMP/build_kaggle.sh" -Value $bs -Encoding ASCII

# ── Zip ──
Write-Host ">>> Zipping..."
if (Test-Path $OUT) { Remove-Item $OUT -Force }
if (Get-Command "7z" -ErrorAction SilentlyContinue) {
    & 7z a -mx1 "$OUT" "$TMP/*" > $null
} else {
    Compress-Archive -Path "$TMP/*" -DestinationPath $OUT
}
$size = (Get-Item $OUT).Length / 1MB
$totalFiles = (Get-ChildItem $TMP -Recurse -File | Measure-Object).Count

Write-Host "=== Done ==="
Write-Host "Output: $OUT ($('{0:F1}' -f $size) MB, $totalFiles files)"
Write-Host ""
Write-Host "=== Usage (Kaggle Notebook) ==="
Write-Host "Cell 1:"
Write-Host '  !cp -r /kaggle/input/YOUR-DATASET/kaggle_full_pack/* /kaggle/working/'
Write-Host '  !cp /kaggle/input/YOUR-DATASET/model.framestore /kaggle/working/'
Write-Host "Cell 2:"
Write-Host '  !bash /kaggle/working/build_kaggle.sh'

Remove-Item $TMP -Recurse -Force
