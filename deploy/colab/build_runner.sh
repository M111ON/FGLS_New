#!/bin/bash
# build_runner.sh — Build POGLS runner with DRamTile + SID on Linux (Colab-ready)
# Usage:
#   bash build_runner.sh                              # default: TinyLlama 1.1B Q4_K_M
#   bash build_runner.sh "https://hf.co/..."          # custom model URL  
#   bash build_runner.sh /path/to/local/model.gguf    # local file
#
# First run: downloads GGUF + builds llama.cpp + creates twin file
# Subsequent runs (if model.tw exists): reuses twin, skips GGUF download
set -euo pipefail

MODEL_SRC="${1:-}"
REPO_URL="https://github.com/M111ON/FGLS_New"
WORK_DIR="/content/FGLS_new"
BUILD_DIR="/content/build"
LLAMA_DIR="/content/llama.cpp"
LLAMA_BUILD_DIR="$LLAMA_DIR/build"
BAKE_DIR="/content/bake_output"

# Default model: TinyLlama 1.1B Q4_K_M (638MB)
DEFAULT_URL="https://huggingface.co/bartowski/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/TinyLlama-1.1B-Chat-v1.0-Q4_K_M.gguf"
DEFAULT_NAME="TinyLlama-1.1B-Chat-v1.0-Q4_K_M.gguf"

echo "=== POGLS Runner Build ==="
echo ""

mkdir -p "$BUILD_DIR" "$BAKE_DIR"

# ── 0. Get source if not present ──
if [ ! -d "$WORK_DIR" ]; then
    echo ">>> Cloning FGLS_new..."
    git clone --depth 1 "$REPO_URL" "$WORK_DIR"
fi
echo "Sources: $WORK_DIR"

# ── 1. Install build deps ──
echo ">>> Installing build dependencies..."
apt-get update -qq && apt-get install -y -qq \
    build-essential cmake git wget curl \
    libomp-dev > /dev/null 2>&1
echo "OK"

# ── 2. Build llama.cpp from source ──
if [ ! -f "$LLAMA_BUILD_DIR/src/libllama.so" ]; then
    echo ">>> Building llama.cpp (first time)..."
    if [ ! -d "$LLAMA_DIR" ]; then
        git clone --depth 1 https://github.com/ggml-org/llama.cpp "$LLAMA_DIR"
    fi
    cd "$LLAMA_DIR"
    mkdir -p build && cd build
    cmake .. -DBUILD_SHARED_LIBS=ON -DLLAMA_CUDA=OFF -DLLAMA_VULKAN=OFF \
        -DLLAMA_METAL=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TESTS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF
    make -j$(nproc) llama 2>&1 | tail -5
    echo "OK ($(ls -lh src/libllama.so | awk '{print $5}'))"
else
    echo "[cached] llama.so found at $LLAMA_BUILD_DIR/src/libllama.so"
fi

# Include paths for llama.cpp
LLAMA_INC="$LLAMA_DIR"
LLAMA_INC_GGML="$LLAMA_DIR/ggml/include"
LLAMA_INC_SRC="$LLAMA_DIR/src"

# ── 3. Check for twin file ──
TWIN_FILE="$BAKE_DIR/model.tw"
if [ -f "$TWIN_FILE" ]; then
    echo ">>> Twin file found at $TWIN_FILE — skipping model download"
    MODEL_PATH="$TWIN_FILE"
else
    # ── 4. Get GGUF model ──
    MODEL_PATH="$MODEL_SRC"
    if [ -z "$MODEL_PATH" ]; then
        MODEL_PATH="$BUILD_DIR/$DEFAULT_NAME"
    fi
    if [ ! -f "$MODEL_PATH" ] || [[ "$MODEL_PATH" != *.gguf ]]; then
        MODEL_URL="$MODEL_SRC"
        if [ -z "$MODEL_URL" ]; then
            MODEL_URL="$DEFAULT_URL"
        fi
        echo ">>> Downloading model..."
        wget -O "$MODEL_PATH" "$MODEL_URL" 2>&1 | tail -3
        echo "OK ($(ls -lh "$MODEL_PATH" | awk '{print $5}'))"
    fi
fi

# ── 5. Compile runner ──
echo ">>> Compiling POGLS runner..."
cd "$WORK_DIR"

INCLUDES=(
    -I"$WORK_DIR"
    -I"$WORK_DIR/collection"
    -I"$WORK_DIR/collection/src"
    -I"$WORK_DIR/collection/core"
    -I"$WORK_DIR/collection/core/core"
    -I"$WORK_DIR/collection/core/pogls_engine/core"
    -I"$WORK_DIR/collection/geopixel"
    -I"$WORK_DIR/collection/geopixel/Metatron/core"
    -I"$WORK_DIR/collection/pogls_engine"
    -I"$WORK_DIR/collection/geo_jump_module/include"
    -I"$WORK_DIR/collection/dgls/diamond/include"
    -I"$LLAMA_INC"
    -I"$LLAMA_INC_GGML"
    -I"$LLAMA_INC_SRC"
)

LIBS=(
    -L"$LLAMA_BUILD_DIR/src" -lllama
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml-base
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml-cpu
    -lm -lpthread -ldl
    -Wl,-rpath,"$LLAMA_BUILD_DIR/src"
    -Wl,-rpath,"$LLAMA_BUILD_DIR/ggml/src"
)

# Step 1: compile C files with gcc
gcc -std=c11 -O2 "${INCLUDES[@]}" -c \
    "$WORK_DIR/runner/llama_pogls_runner_sid_v2.c" \
    -o "$BUILD_DIR/runner.o" 2>&1

# Step 2: compile geo_jump.c with gcc
gcc -std=c11 -O2 "${INCLUDES[@]}" -c \
    "$WORK_DIR/collection/geo_jump_module/src/geo_jump.c" \
    -o "$BUILD_DIR/geo_jump.o" 2>&1

# Step 3: compile C++ file with g++
g++ -std=c++17 -O2 "${INCLUDES[@]}" -c \
    "$WORK_DIR/runner/kv_tensor_access.cpp" \
    -o "$BUILD_DIR/kv_tensor_access.o" 2>&1

# Step 4: link with g++
g++ "$BUILD_DIR/runner.o" "$BUILD_DIR/geo_jump.o" \
    "$BUILD_DIR/kv_tensor_access.o" \
    "${LIBS[@]}" -lstdc++ -o "$BUILD_DIR/pogls_runner" 2>&1

echo "OK ($(ls -lh "$BUILD_DIR/pogls_runner" | awk '{print $5}'))"

# ── 6. Run inference ──
if [ -f "$TWIN_FILE" ]; then
    # Reopen twin — no GGUF needed for weights
    echo ""
    echo ">>> Running inference with twin file (no GGUF weight load)..."
    LD_LIBRARY_PATH="$LLAMA_BUILD_DIR/src:$LLAMA_BUILD_DIR/ggml/src" \
    "$BUILD_DIR/pogls_runner" "$MODEL_PATH" \
        --prompt "Hi" --max-new 16 \
        --dramtile-file "$TWIN_FILE" \
        --sid-face 1 --ngl 0 2>&1
else
    # First run: create twin file from GGUF
    echo ""
    echo ">>> First run — creating twin file..."
    LD_LIBRARY_PATH="$LLAMA_BUILD_DIR/src:$LLAMA_BUILD_DIR/ggml/src" \
    "$BUILD_DIR/pogls_runner" "$MODEL_PATH" \
        --prompt "Hi" --max-new 16 \
        --dramtile-file "$TWIN_FILE" \
        --sid-face 1 --ngl 0 2>&1

    # Verify twin was created
    if [ -f "$TWIN_FILE" ]; then
        echo ""
        echo ">>> Twin file created: $TWIN_FILE ($(ls -lh "$TWIN_FILE" | awk '{print $5}'))"
        echo ">>> Next run will skip GGUF weight load entirely."
    fi
fi

echo ""
echo "=== DONE ==="
echo "Runner: $BUILD_DIR/pogls_runner"
echo "Twin:   $TWIN_FILE"
ls -lh "$BUILD_DIR/pogls_runner" "$TWIN_FILE" 2>/dev/null || true
