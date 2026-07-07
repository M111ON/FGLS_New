#!/bin/bash
# build_runner.sh — Build POGLS runner with DRamTile + SID + FrameStore on Linux (Colab/Kaggle-ready)
# Usage:
#   bash build_runner.sh                              # default: TinyLlama 1.1B Q4_K_M (CPU)
#   bash build_runner.sh "https://hf.co/..."          # custom model URL
#   bash build_runner.sh /path/to/local/model.gguf    # local file
#   bash build_runner.sh ... --cuda                   # enable CUDA (T4/V100/A100)
#   bash build_runner.sh ... --framestore             # use FrameStore instead of twin/dramtile
#
# Outputs:
#   /content/build/pogls_runner           — the runner binary
#   /content/build/gguf_to_framestore     — FrameStore converter
#   /content/bake_output/model.framestore — FrameStore data file (with --framestore)
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

# Parse flags
USE_CUDA=0
USE_FRAMESTORE=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --cuda) USE_CUDA=1 ;;
        --framestore) USE_FRAMESTORE=1 ;;
        *) ARGS+=("$arg") ;;
    esac
done
MODEL_SRC="${ARGS[0]:-}"

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

# CUDA toolkit detection
if [ "$USE_CUDA" -eq 1 ]; then
    if command -v nvcc &>/dev/null; then
        echo "[cuda] nvcc found at $(which nvcc)"
    else
        echo "[cuda] installing nvidia-cuda-toolkit..."
        apt-get install -y -qq nvidia-cuda-toolkit > /dev/null 2>&1 || true
        if command -v nvcc &>/dev/null; then
            echo "[cuda] OK ($(nvcc --version | grep release))"
        else
            echo "[cuda] WARNING: nvcc not found, T4 may not be detected; fallback to CPU"
            USE_CUDA=0
        fi
    fi
fi
echo "OK"

# ── 2. Build llama.cpp from source ──
LLAMA_BUILD_NEEDED=0
if [ ! -f "$LLAMA_BUILD_DIR/src/libllama.so" ] || [ ! -f "$LLAMA_BUILD_DIR/src/libllama.so" ]; then
    LLAMA_BUILD_NEEDED=1
else
    # Check if we need to rebuild for CUDA
    if [ "$USE_CUDA" -eq 1 ]; then
        CUDA_SYMS=$(nm -D "$LLAMA_BUILD_DIR/src/libllama.so" 2>/dev/null | grep -c "cuda" || true)
        if [ "$CUDA_SYMS" -lt 5 ]; then
            echo "[cuda] existing build is CPU-only, rebuilding with CUDA..."
            LLAMA_BUILD_NEEDED=1
        fi
    fi
fi

if [ "$LLAMA_BUILD_NEEDED" -eq 1 ]; then
    echo ">>> Building llama.cpp..."
    if [ ! -d "$LLAMA_DIR" ]; then
        git clone --depth 1 https://github.com/ggml-org/llama.cpp "$LLAMA_DIR"
    fi
    cd "$LLAMA_DIR"
    mkdir -p build && cd build

    CMAKE_FLAGS=(
        -DBUILD_SHARED_LIBS=ON
        -DLLAMA_VULKAN=OFF
        -DLLAMA_METAL=OFF
        -DLLAMA_BUILD_SERVER=OFF
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_TOOLS=OFF
    )
    if [ "$USE_CUDA" -eq 1 ]; then
        CMAKE_FLAGS+=(-DLLAMA_CUDA=ON -DLLAMA_CUDA_F16=ON)
    else
        CMAKE_FLAGS+=(-DLLAMA_CUDA=OFF)
    fi

    cmake .. "${CMAKE_FLAGS[@]}"
    make -j$(nproc) llama 2>&1 | tail -5
    echo "OK ($(ls -lh src/libllama.so | awk '{print $5}'))"
else
    echo "[cached] llama.so found at $LLAMA_BUILD_DIR/src/libllama.so"
fi

# Include paths for llama.cpp
LLAMA_INC="$LLAMA_DIR"
LLAMA_INC_GGML="$LLAMA_DIR/ggml/include"
LLAMA_INC_SRC="$LLAMA_DIR/src"

# ── 3. Get GGUF model ──
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
    -I"$WORK_DIR/collection/rdh"
    -I"$LLAMA_INC"
    -I"$LLAMA_INC_GGML"
    -I"$LLAMA_INC_SRC"
)

# CUDA runtime libs
CUDA_LIBS=()
if [ "$USE_CUDA" -eq 1 ]; then
    CUDA_LIBS=(-lcuda -lcudart)
fi

LIBS=(
    -L"$LLAMA_BUILD_DIR/src" -lllama
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml-base
    -L"$LLAMA_BUILD_DIR/ggml/src" -lggml-cpu
    "${CUDA_LIBS[@]}"
    -lm -lpthread -ldl
    -Wl,-rpath,"$LLAMA_BUILD_DIR/src"
    -Wl,-rpath,"$LLAMA_BUILD_DIR/ggml/src"
)

# Step 1: compile C files with gcc
echo ">>> Compiling runner core..."
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

# ── 6. Build FrameStore converter ──
echo ">>> Building FrameStore converter..."
gcc -std=c11 -O2 "${INCLUDES[@]}" \
    "$WORK_DIR/runner/gguf_to_pogls_v3_framestore.c" \
    -o "$BUILD_DIR/gguf_to_framestore" -lm 2>&1
echo "OK ($(ls -lh "$BUILD_DIR/gguf_to_framestore" | awk '{print $5}'))"

# ── 7. Convert to FrameStore (if --framestore) ──
FS_FILE="$BAKE_DIR/model.framestore"
if [ "$USE_FRAMESTORE" -eq 1 ]; then
    if [ -f "$FS_FILE" ]; then
        echo ">>> FrameStore found at $FS_FILE — skipping conversion"
    else
        echo ">>> Converting model to FrameStore..."
        "$BUILD_DIR/gguf_to_framestore" "$MODEL_PATH" "$FS_FILE" --verify 2>&1
        echo "OK ($(ls -lh "$FS_FILE" | awk '{print $5}'))"
    fi
fi

# ── 8. Run inference ──
NGL_FLAG="--ngl 0"
if [ "$USE_CUDA" -eq 1 ]; then
    NGL_FLAG="--ngl 99"
fi

echo ""
echo ">>> Running inference..."
LD_LIBRARY_PATH="$LLAMA_BUILD_DIR/src:$LLAMA_BUILD_DIR/ggml/src" \
"$BUILD_DIR/pogls_runner" "$MODEL_PATH" \
    --prompt "Hello" --max-new 16 \
    --sid-disable \
    ${USE_FRAMESTORE:+--pogls-fs "$FS_FILE"} \
    $NGL_FLAG 2>&1 | tail -20

echo ""
echo "=== DONE ==="
echo "Runner: $BUILD_DIR/pogls_runner"
if [ "$USE_FRAMESTORE" -eq 1 ]; then
    echo "FrameStore: $FS_FILE"
fi
ls -lh "$BUILD_DIR/pogls_runner" "$BUILD_DIR/gguf_to_framestore" $([ "$USE_FRAMESTORE" -eq 1 ] && echo "$FS_FILE") 2>/dev/null || true
