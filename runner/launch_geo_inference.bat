@echo off
:: ═══════════════════════════════════════════════════════════════
:: FGLS Geo-Inference Stack
:: Launches llama-server + POGLS geometry layer
:: ═══════════════════════════════════════════════════════════════
setlocal

:: ── Config ────────────────────────────────────────────────────
set LLAMA_DIR=I:\llama\llama-b9733-bin-win-vulkan-x64
set MODEL=I:\model\Qwen3-0.6B-Q4_0.gguf
set NGL=99
set CTX=8192
set PORT=8080
set GEO_PORT=8081
set THREADS=4

:: ── Launch llama-server ───────────────────────────────────────
echo.
echo  ╔══════════════════════════════════════════════╗
echo  ║  FGLS Geo-Inference Stack                    ║
echo  ╠══════════════════════════════════════════════╣
echo  ║  Model: Qwen3-0.6B Q4_0 (112 t/s)           ║
echo  ║  GPU:   GTX 1050 Ti (Vulkan)                ║
echo  ║  CTX:   8192 tokens                         ║
echo  ╚══════════════════════════════════════════════╝
echo.
echo Starting llama-server on port %PORT%...
echo.

start "llama-server" "%LLAMA_DIR%\llama-server.exe" ^
    -m "%MODEL%" ^
    -ngl %NGL% ^
    -c %CTX% ^
    -t %THREADS% ^
    --host 127.0.0.1 ^
    --port %PORT% ^
    --chat-template chatml

:: ── Wait for server to be ready ───────────────────────────────
echo Waiting for server to start...
timeout /t 3 /nobreak > nul

:: ── Launch POGLS geometry server ──────────────────────────────
echo Starting POGLS geometry layer on port %GEO_PORT%...
echo.

cd /d I:\FGLS_new\collection\core\pogls_engine
python -m uvicorn rest_server_s58:app --host 127.0.0.1 --port %GEO_PORT%

:: ── Done ──────────────────────────────────────────────────────
echo.
echo Stack ready!
echo   LLM:      http://127.0.0.1:%PORT%/v1/chat/completions
echo   Geometry: http://127.0.0.1:%GEO_PORT%/docs
echo   Browser:  http://127.0.0.1:%PORT%
echo.
pause
