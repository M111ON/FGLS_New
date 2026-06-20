#!/bin/bash
# build_colab.sh — Bake GGUF model using standalone bake_tool (no llama.cpp needed)
# Usage:
#   bash build_colab.sh                              # default: TinyLlama 1.1B Q4_K_M
#   bash build_colab.sh "https://hf.co/..."          # custom model URL
#   bash build_colab.sh /path/to/local/model.gguf    # local file
#
# Output: ./bake_output/capture-*.tw + store-*.gsten
set -euo pipefail

MODEL_SRC="${1:-}"
WORK_DIR="/content/FGLS_new"
BUILD_DIR="/content/build"
BAKE_DIR="/content/bake_output"

# Default model: TinyLlama 1.1B Q4_K_M (638MB)
DEFAULT_URL="https://huggingface.co/bartowski/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/TinyLlama-1.1B-Chat-v1.0-Q4_K_M.gguf"
DEFAULT_NAME="TinyLlama-1.1B-Chat-v1.0-Q4_K_M.gguf"

echo "=== FGLS_new Bake Tool ==="
echo ""

mkdir -p "$BUILD_DIR" "$BAKE_DIR"

# ── 1. Install deps (just gcc + wget) ──
echo ">>> Installing build dependencies..."
apt-get update -qq && apt-get install -y -qq gcc wget curl > /dev/null 2>&1
echo "OK"

# ── 2. Source check ──
echo ">>> Checking FGLS_new sources..."
if [ ! -d "$WORK_DIR" ]; then
    echo "ERROR: $WORK_DIR not found!"
    echo "Upload FGLS_new/ to /content/ or mount Google Drive"
    exit 1
fi
cd "$WORK_DIR"
WORK_DIR="$(pwd)"
echo "OK at $WORK_DIR"

# ── 3. Compile bake_tool (1 second, no llama.cpp) ──
echo ">>> Compiling bake_tool..."
gcc -O2 -std=c11 -Wno-unused-result \
    -I"$WORK_DIR" \
    -I"$WORK_DIR/collection" \
    -I"$WORK_DIR/collection/src" \
    -I"$WORK_DIR/collection/core" \
    -I"$WORK_DIR/collection/core/core" \
    -I"$WORK_DIR/collection/core/pogls_engine/core" \
    -I"$WORK_DIR/collection/core/geo_headers" \
    -I"$WORK_DIR/collection/geo_jump_module/include" \
    -o "$BUILD_DIR/bake_tool" \
    "$WORK_DIR/runner/bake_tool.c" \
    "$WORK_DIR/collection/geo_jump_module/src/geo_jump.c" \
    -lm 2>&1
echo "OK ($(ls -lh "$BUILD_DIR/bake_tool" | awk '{print $5}'))"

echo ">>> Compiling gsten_inspect..."
gcc -O2 -std=c11 -Wno-unused-result \
    -I"$WORK_DIR" \
    -I"$WORK_DIR/collection" \
    -I"$WORK_DIR/collection/src" \
    -I"$WORK_DIR/collection/core" \
    -I"$WORK_DIR/collection/core/core" \
    -I"$WORK_DIR/collection/core/pogls_engine/core" \
    -I"$WORK_DIR/collection/core/geo_headers" \
    -I"$WORK_DIR/collection/geo_jump_module/include" \
    -o "$BUILD_DIR/gsten_inspect" \
    "$WORK_DIR/runner/gsten_inspect.c" \
    "$WORK_DIR/collection/geo_jump_module/src/geo_jump.c" \
    -lm 2>&1
echo "OK ($(ls -lh "$BUILD_DIR/gsten_inspect" | awk '{print $5}'))"

# ── 4. Get model ──
MODEL_PATH="$MODEL_SRC"
if [ -z "$MODEL_PATH" ]; then
    MODEL_PATH="$BUILD_DIR/$DEFAULT_NAME"
fi

if [ ! -f "$MODEL_PATH" ]; then
    MODEL_URL="$MODEL_SRC"
    if [ -z "$MODEL_URL" ]; then
        MODEL_URL="$DEFAULT_URL"
    fi
    echo ""
    echo ">>> Downloading model..."
    wget -O "$MODEL_PATH" "$MODEL_URL" 2>&1 | tail -3
    echo "OK ($(ls -lh "$MODEL_PATH" | awk '{print $5}'))"
fi

# ── 5. Bake! ──
echo ""
echo ">>> Running bake_tool..."
"$BUILD_DIR/bake_tool" "$MODEL_PATH" "$BAKE_DIR" 2>&1

# ── 6. Inspect ──
echo ""
echo ">>> Tensor → Node ID mapping (top 30):"
"$BUILD_DIR/gsten_inspect" "$BAKE_DIR/store.gsten" 2>&1 | head -35

# ── 7. Done ──
echo ""
echo "=== BAKE COMPLETE ==="
ls -lh "$BAKE_DIR"/
echo ""
echo "Download from Colab:"
echo '  from google.colab import files'
echo '  import glob'
echo '  for f in glob.glob("/content/bake_output/*"):'
echo '    files.download(f)'
