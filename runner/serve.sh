#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# FGLS Inference Server — OpenAI-compatible API
# Usage: ./serve.sh [model_key] [port]
# ═══════════════════════════════════════════════════════════════

LLAMA="I:/llama/llama-b9733-bin-win-vulkan-x64/llama-server.exe"
MODEL="I:/model/Qwen3-0.6B-Q4_0.gguf"
NGL=99
CTX=16384
THREADS=2
PORT="${2:-8080}"

# Model selection
case "${1:-}" in
  q3q4)  MODEL="I:/model/Qwen3-0.6B-Q4_0.gguf" ;;
  q3q8)  MODEL="I:/model/Qwen3-0.6B-Q8_0.gguf" ;;
  q25)   MODEL="I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf" ;;
  smol)  MODEL="I:/model/SmolLM2-360M-Instruct.Q8_0.gguf" ;;
  "")    MODEL="I:/model/Qwen3-0.6B-Q4_0.gguf" ;;
  *)     MODEL="$1" ;;
esac

echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║  FGLS Inference Server                       ║"
echo "╠══════════════════════════════════════════════╣"
echo "║  Model:  $(basename "$MODEL")"
echo "║  GPU:    GTX 1050 Ti (Vulkan, ngl=$NGL)"
echo "║  CTX:    $CTX tokens"
echo "║  Port:   $PORT"
echo "╚══════════════════════════════════════════════╝"
echo ""
echo "API endpoint: http://127.0.0.1:$PORT/v1/chat/completions"
echo "Browser chat: http://127.0.0.1:$PORT"
echo ""
echo "Press Ctrl+C to stop"
echo ""

"$LLAMA" \
  -m "$MODEL" \
  -ngl $NGL \
  -c $CTX \
  -t $THREADS \
  --host 127.0.0.1 \
  --port $PORT \
  --chat-template chatml
