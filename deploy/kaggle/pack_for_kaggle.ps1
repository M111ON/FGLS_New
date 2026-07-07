# pack_for_kaggle.ps1 — สร้าง zip สำหรับ Kaggle (source + build script)
# ใช้: pwsh deploy/kaggle/pack_for_kaggle.ps1
#
# Output: deploy/kaggle/kaggle_framestore_pack.zip
# อัปโหลด zip นี้ + .framestore ไปที่ Kaggle Dataset

$ErrorActionPreference = "Stop"
$ROOT = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OUT = Join-Path $PSScriptRoot "kaggle_framestore_pack.zip"

# ไฟล์ที่ต้องใช้
$files = @(
    "runner/pogls_v3_framestore.h",
    "runner/gguf_to_pogls_v3_framestore.c",
    "runner/gguf_index.h",
    "runner/addr_space.h",
    "runner/llama_pogls_runner_sid_v2.c",
    "runner/kv_tensor_access.cpp",
    "runner/kv_tensor_access.h",
    "runner/geo_addr.h",
    "collection/rdh/rdh_addr.h",
    "collection/core/core/pogls_platform.h",
    "collection/core/core/pogls_fold.h",
    "deploy/colab/build_runner.sh"
)

# สร้าง temp directory layout
$TMP = Join-Path $env:TMP "kaggle_framestore_$(Get-Random)"
New-Item -ItemType Directory -Path $TMP -Force | Out-Null

foreach ($f in $files) {
    $src = Join-Path $ROOT $f
    $dst = Join-Path $TMP $f
    $parent = Split-Path -Parent $dst
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst
}

# สร้าง notebook script
$notebook = @'
#!/bin/bash
# framestore_kaggle.sh — รันบน Kaggle Notebook โดยตรง
# วางไฟล์นี้ + model.framestore ใน /kaggle/working/
set -euo pipefail

WORK="/kaggle/working"
BUILD="$WORK/build"
LLAMA_DIR="$WORK/llama.cpp"
FS_FILE="$WORK/model.framestore"

# 1. Install deps
echo ">>> Installing dependencies..."
apt-get update -qq && apt-get install -y -qq build-essential cmake wget curl libomp-dev nvidia-cuda-toolkit > /dev/null 2>&1

# 2. Get llama.cpp source
echo ">>> Getting llama.cpp source..."
LLAMA_URL="https://github.com/ggml-org/llama.cpp"
if git clone --depth 1 "$LLAMA_URL" "$LLAMA_DIR" 2>/dev/null; then
    echo "OK (git clone)"
else
    echo "[fallback] trying wget..."
    wget -q "${LLAMA_URL}/archive/refs/heads/master.tar.gz" -O /tmp/llama.tar.gz
    mkdir -p "$LLAMA_DIR" && tar xzf /tmp/llama.tar.gz -C "$LLAMA_DIR" --strip=1
    echo "OK (wget)"
fi

# 3. Build llama.cpp with CUDA
echo ">>> Building llama.cpp (CUDA)..."
cd "$LLAMA_DIR" && mkdir -p build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DLLAMA_CUDA=ON -DLLAMA_CUDA_F16=ON \
    -DLLAMA_VULKAN=OFF -DLLAMA_METAL=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF
make -j$(nproc) llama 2>&1 | tail -3
echo "OK"

# 4. Build runner
echo ">>> Building runner..."
mkdir -p "$BUILD"
INCS="-I$WORK -I$WORK/collection -I$WORK/collection/rdh -I$WORK/collection/core/core -I$LLAMA_DIR -I$LLAMA_DIR/ggml/include -I$LLAMA_DIR/src"
gcc -std=c11 -O2 $INCS -c "$WORK/runner/llama_pogls_runner_sid_v2.c" -o "$BUILD/runner.o"
g++ -std=c++17 -O2 $INCS -c "$WORK/runner/kv_tensor_access.cpp" -o "$BUILD/kv_tensor_access.o"
g++ "$BUILD/runner.o" "$BUILD/kv_tensor_access.o" \
    -L"$LLAMA_DIR/build/src" -lllama \
    -L"$LLAMA_DIR/build/ggml/src" -lggml -lggml-base -lggml-cpu \
    -lcuda -lcudart \
    -lm -lpthread -ldl -lstdc++ \
    -Wl,-rpath,"$LLAMA_DIR/build/src" -Wl,-rpath,"$LLAMA_DIR/build/ggml/src" \
    -o "$BUILD/pogls_runner"
echo "OK ($(ls -lh "$BUILD/pogls_runner" | awk '{print $5}'))"

# 5. Run inference
echo ""
echo ">>> Running inference..."
LD_LIBRARY_PATH="$LLAMA_DIR/build/src:$LLAMA_DIR/build/ggml/src" \
"$BUILD/pogls_runner" "$FS_FILE" \
    --pogls-fs "$FS_FILE" --sid-disable --ngl 99 \
    --prompt "Hello" --max-new 16 --temp 0 2>&1 | tail -10

echo ""
echo "=== DONE ==="
'@

$scriptPath = Join-Path $TMP "framestore_kaggle.sh"
Set-Content -Path $scriptPath -Value $notebook -Encoding ASCII

# ไฟล์ README
$readme = @'
# Kaggle FrameStore Runner
1. อัปโหลด kaggle_framestore_pack.zip เป็น Kaggle Dataset
2. อัปโหลด model.framestore (ไฟล์ .framestore ที่ convert ไว้แล้ว) เป็น Kaggle Dataset เดียวกัน
3. สร้าง Notebook ใหม่ ใช้ GPU Accelerator T4
4. รัน:

!cp -r /kaggle/input/YOUR-DATASET/kaggle_framestore_pack/* /kaggle/working/
!cp /kaggle/input/YOUR-DATASET/model.framestore /kaggle/working/
!bash /kaggle/working/framestore_kaggle.sh
'@

Set-Content -Path (Join-Path $TMP "README.txt") -Value $readme -Encoding ASCII

# Zip ทุกอย่าง
if (Test-Path $OUT) { Remove-Item $OUT -Force }
Compress-Archive -Path (Join-Path $TMP "*") -DestinationPath $OUT

Write-Host "=== Packed ==="
Write-Host "Output: $OUT"
Write-Host "Size: $((Get-Item $OUT).Length / 1MB) MB"
Write-Host ""
Write-Host "Steps:"
Write-Host "  1. อัปโหลด $OUT + model.framestore ไปที่ Kaggle Dataset"
Write-Host "  2. สร้าง Notebook (GPU T4) → รันคำสั่งด้านบน"

Remove-Item -Path $TMP -Recurse -Force
