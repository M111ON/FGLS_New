#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# FGLS Benchmark — Compare all models on this hardware
# Usage: ./bench.sh [model_key]
# ═══════════════════════════════════════════════════════════════

BENCH="I:/llama/llama-b9733-bin-win-vulkan-x64/llama-bench.exe"
NGL=99
THREADS=4

echo ""
echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║  FGLS Inference Benchmark                                    ║"
echo "║  Hardware: GTX 1050 Ti (4GB) × 2, Vulkan                    ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

run_bench() {
  local name="$1"
  local model="$2"
  echo "--- $name ---"
  "$BENCH" -m "$model" -t $THREADS -ngl $NGL 2>&1 | grep -E "pp512|tg128"
  echo ""
}

if [ -n "${1:-}" ]; then
  # Single model benchmark
  case "$1" in
    q3q4)  run_bench "Qwen3-0.6B Q4_0" "I:/model/Qwen3-0.6B-Q4_0.gguf" ;;
    q3q8)  run_bench "Qwen3-0.6B Q8_0" "I:/model/Qwen3-0.6B-Q8_0.gguf" ;;
    q25)   run_bench "Qwen2.5-0.5B Q8_0" "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf" ;;
    smol)  run_bench "SmolLM2-360M Q8_0" "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf" ;;
    *)     run_bench "$1" "$1" ;;
  esac
else
  # Full benchmark
  run_bench "Qwen3-0.6B Q4_0 (359MB)" "I:/model/Qwen3-0.6B-Q4_0.gguf"
  run_bench "Qwen3-0.6B Q8_0 (604MB)" "I:/model/Qwen3-0.6B-Q8_0.gguf"
  run_bench "Qwen2.5-0.5B Q8_0 (639MB)" "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"
  run_bench "SmolLM2-360M Q8_0 (367MB)" "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "PP = Prompt Processing (prefill) | TG = Text Generation (decode)"
echo "═══════════════════════════════════════════════════════════════"
