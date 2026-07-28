@echo off
:: ═══════════════════════════════════════════════════════════════
:: Quick Chat — One-shot chat with Qwen3 (no server)
:: ═══════════════════════════════════════════════════════════════
setlocal

set LLAMA=I:\llama\llama-b9733-bin-win-vulkan-x64\llama-cli.exe
set MODEL=I:\model\Qwen3-0.6B-Q4_0.gguf

:: ── Parse args ────────────────────────────────────────────────
if "%~1"=="" (
    echo Usage: quick_chat.bat "your message here"
    echo Example: quick_chat.bat "สวัสดีครับ ช่วยแนะนำตัวหน่อย"
    echo.
    echo Models:
    echo   q3q4  = Qwen3-0.6B Q4_0 (default, 112 t/s)
    echo   q3q8  = Qwen3-0.6B Q8_0 (85 t/s)
    echo   q25   = Qwen2.5-0.5B Q8_0 (82 t/s)
    echo   smol  = SmolLM2-360M Q8_0 (89 t/s)
    goto :eof
)

if "%~1"=="q3q8" (
    set MODEL=I:\model\Qwen3-0.6B-Q8_0.gguf
    shift
)
if "%~1"=="q25" (
    set MODEL=I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf
    shift
)
if "%~1"=="smol" (
    set MODEL=I:\model\SmolLM2-360M-Instruct.Q8_0.gguf
    shift
)

:: ── Chat ──────────────────────────────────────────────────────
"%LLAMA%" -m "%MODEL%" -t 4 -ngl 99 -n 150 --no-display-prompt ^
    -p "<|im_start|>user
%~1
<|im_start|>assistant
" 2>&1 | findstr /V "^>" | findstr /V "Loading model" | findstr /V "build" | findstr /V "model" | findstr /V "modalities" | findstr /V "available commands" | findstr /V "regen" | findstr /V "clear" | findstr /V "read" | findstr /V "glob" | findstr /V "Prompt:" | findstr /V "Generation:"
