#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# FGLS Inference — Quick Chat
# Usage: ./chat.sh "message"
#        ./chat.sh q3q8 "message"
#        ./chat.sh cpu "message"    (CPU-only, no GPU needed)
# ═══════════════════════════════════════════════════════════════

LLAMA="I:/llama/llama-b9733-bin-win-vulkan-x64/llama-cli.exe"
MODEL="I:/model/Qwen3-0.6B-Q4_0.gguf"
NGL=99
THREADS=2
MAX_TOKENS=150

# Model selection
case "${1:-}" in
  q3q4)  MODEL="I:/model/Qwen3-0.6B-Q4_0.gguf";  shift ;;
  q3q8)  MODEL="I:/model/Qwen3-0.6B-Q8_0.gguf";  shift ;;
  q25)   MODEL="I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"; shift ;;
  smol)  MODEL="I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"; shift ;;
  cpu)   NGL=0; shift ;;
  bench) exec "$LLAMA" -m "$MODEL" -t $THREADS -ngl $NGL; exit ;;
  -h|--help|"")
    echo "Usage: ./chat.sh [model] \"message\""
    echo ""
    echo "Models: q3q4 (default) | q3q8 | q25 | smol | cpu | bench"
    echo ""
    echo "Examples:"
    echo "  ./chat.sh \"สวัสดีครับ\""
    echo "  ./chat.sh q3q8 \"1+1 เท่ากับอะไร\""
    echo "  ./chat.sh cpu \"Hello\"     # no GPU needed"
    echo "  ./chat.sh bench            # run benchmark"
    exit 0 ;;
esac

MSG="${1:-}"
if [ -z "$MSG" ]; then
  echo "Error: no message"
  exit 1
fi

"$LLAMA" \
  -m "$MODEL" \
  -t $THREADS \
  -ngl $NGL \
  -n $MAX_TOKENS \
  --no-display-prompt \
  -p "<|im_start|>user
${MSG}
<|im_start|>assistant
" 2>&1 | grep -v "^>" | grep -v "Loading\|build \|^model \|^modalities\|commands\|/regen\|/clear\|/read\|/glob\|Prompt:\|Generation:\|^\[" | grep -v "^$" | grep -v "^▄\|^██\|▄▄\|▀▀\|█" | head -20
