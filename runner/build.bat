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
set DEFS=-DGGML_SHARED -DPOGLS_NO_VULKAN -DPOGLS_USE_ZSTD

echo === Compile C (llama_pogls_runner_sid_v2.c) ===
gcc.exe -m64 -O2 -std=c11 %INCS% %DEFS% -c llama_pogls_runner_sid_v2.c -o llama_pogls_runner_sid_v2_new.o 2>build_err.txt
set E=%ERRORLEVEL%
if exist build_err.txt for %%f in (build_err.txt) do if %%~zf gtr 0 type build_err.txt
if !E! NEQ 0 echo COMPILE FAILED=!E! && popd && exit /b !E!

echo === Compile C (geo_jump.c) ===
gcc.exe -m64 -O2 -std=c11 %INCS% %DEFS% -c ..\collection\geo_jump_module\src\geo_jump.c -o geo_jump_new.o 2>build_err2.txt
set E=!ERRORLEVEL!
if exist build_err2.txt for %%f in (build_err2.txt) do if %%~zf gtr 0 type build_err2.txt
if !E! NEQ 0 echo GEO_JUMP FAILED=!E! && popd && exit /b !E!

echo === Compile C++ (kv_tensor_access.cpp) ===
g++.exe -m64 -O2 -std=c++17 %INCS% %DEFS% -c kv_tensor_access.cpp -o kv_tensor_access_new.o 2>build_err3.txt
set E=!ERRORLEVEL!
if exist build_err3.txt for %%f in (build_err3.txt) do if %%~zf gtr 0 type build_err3.txt
if !E! NEQ 0 echo KV_TENSOR FAILED=!E! && popd && exit /b !E!

echo === Link ===
gcc.exe -m64 -O2 -Wl,--stack,16777216 -o runner_test_new.exe llama_pogls_runner_sid_v2_new.o geo_jump_new.o kv_tensor_access_new.o -L. -llibllama -lggml -lggml-base -lggml-cpu-x64 -lggml-vulkan -lstdc++ -lm ./zstd.dll 2>build_err_link.txt
set E=!ERRORLEVEL!
if exist build_err_link.txt for %%f in (build_err_link.txt) do if %%~zf gtr 0 type build_err_link.txt
if !E! NEQ 0 (echo LINK FAILED=!E!) else (echo LINK OK - runner_test_new.exe created)

popd
