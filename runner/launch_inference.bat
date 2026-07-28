@echo off
:: ═══════════════════════════════════════════════════════════════
:: FGLS Inference Stack Launcher
:: Launches llama-server + browser chat UI
:: ═══════════════════════════════════════════════════════════════
setlocal

:: ── Config ────────────────────────────────────────────────────
set LLAMA_DIR=I:\llama\llama-b9733-bin-win-vulkan-x64
set MODEL=I:\model\Qwen3-0.6B-Q4_0.gguf
set NGL=99
set CTX=8192
set PORT=8080
set THREADS=4

:: ── Menu ──────────────────────────────────────────────────────
echo.
echo  ╔══════════════════════════════════════════════╗
echo  ║  FGLS Inference Stack                        ║
echo  ╠══════════════════════════════════════════════╣
echo  ║  1. Qwen3-0.6B Q4_0  (359MB, 112 t/s)      ║
echo  ║  2. Qwen3-0.6B Q8_0  (604MB,  85 t/s)      ║
echo  ║  3. Qwen2.5-0.5B Q8_0 (639MB,  82 t/s)     ║
echo  ║  4. SmolLM2-360M Q8_0 (367MB,  89 t/s)     ║
echo  ║  5. Custom model                            ║
echo  ║  6. Benchmark all models                    ║
echo  ╚══════════════════════════════════════════════╝
echo.
set /p CHOICE="Select model [1-6]: "

if "%CHOICE%"=="1" set MODEL=I:\model\Qwen3-0.6B-Q4_0.gguf
if "%CHOICE%"=="2" set MODEL=I:\model\Qwen3-0.6B-Q8_0.gguf
if "%CHOICE%"=="3" set MODEL=I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf
if "%CHOICE%"=="4" set MODEL=I:\model\SmolLM2-360M-Instruct.Q8_0.gguf
if "%CHOICE%"=="5" (
    set /p MODEL="Enter model path: "
)
if "%CHOICE%"=="6" goto :benchmark

:: ── Launch server ─────────────────────────────────────────────
echo.
echo Starting llama-server...
echo   Model: %MODEL%
echo   GPU layers: %NGL%
echo   Context: %CTX%
echo   Port: %PORT%
echo.
echo Open browser: http://127.0.0.1:%PORT%
echo.

"%LLAMA_DIR%\llama-server.exe" ^
    -m "%MODEL%" ^
    -ngl %NGL% ^
    -c %CTX% ^
    -t %THREADS% ^
    --host 127.0.0.1 ^
    --port %PORT% ^
    --chat-template chatml

goto :eof

:: ── Benchmark mode ────────────────────────────────────────────
:benchmark
echo.
echo Running benchmark on all models...
echo.

echo --- Qwen3-0.6B Q4_0 ---
"%LLAMA_DIR%\llama-bench.exe" -m I:\model\Qwen3-0.6B-Q4_0.gguf -t %THREADS% -ngl %NGL% 2>&1 | findstr "t/s"

echo.
echo --- Qwen3-0.6B Q8_0 ---
"%LLAMA_DIR%\llama-bench.exe" -m I:\model\Qwen3-0.6B-Q8_0.gguf -t %THREADS% -ngl %NGL% 2>&1 | findstr "t/s"

echo.
echo --- Qwen2.5-0.5B Q8_0 ---
"%LLAMA_DIR%\llama-bench.exe" -m I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf -t %THREADS% -ngl %NGL% 2>&1 | findstr "t/s"

echo.
echo --- SmolLM2-360M Q8_0 ---
"%LLAMA_DIR%\llama-bench.exe" -m I:\model\SmolLM2-360M-Instruct.Q8_0.gguf -t %THREADS% -ngl %NGL% 2>&1 | findstr "t/s"

echo.
echo Done!
pause
