@echo off
setlocal ENABLEDELAYEDEXPANSION
set PATH=C:\msys64\mingw64\bin;%PATH%
set RUNNER=%~dp0
pushd %RUNNER%

set INCS=-I. -I..\collection -I..\collection\src -I..\collection\core
set INCS=%INCS% -I..\collection\core\core -I..\collection\geopixel
set INCS=%INCS% -I..\collection\geopixel\Metatron\core -I..\collection\pogls_engine
set INCS=%INCS% -I..\collection\geo_jump_module\include
set INCS=%INCS% -I..\collection\geopixel\hbv_bundle\Diamond_shell_encoder
set INCS=%INCS% -I..\collection\geopixel\hbv_bundle\Diamond_decode_hamburger
set INCS=%INCS% -I..\collection\geopixel\hbv_bundle\core -I..\collection\Hfolder
set INCS=%INCS% -II:/llama.cpp/include -II:/llama.cpp/ggml/include -II:/llama.cpp/src

echo === gguf_to_pogls ===
gcc.exe -m64 -O2 -std=c11 %INCS% -DPOGLS_USE_ZSTD -Wl,--stack,16777216 -o gguf_to_pogls_new.exe gguf_to_pogls.c ./zstd.dll -lm 2>build_gguf.txt
set E=!ERRORLEVEL!
if exist build_gguf.txt for %%f in (build_gguf.txt) do if %%~zf gtr 0 type build_gguf.txt
if !E! NEQ 0 (echo BUILD FAILED=!E!) else (echo gguf_to_pogls_new.exe OK)

popd
