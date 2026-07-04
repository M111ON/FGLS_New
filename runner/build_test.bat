@echo off
setlocal
set PATH=C:\msys64\mingw64\bin;%PATH%
gcc.exe -m64 -O2 -std=c11 -I. -I.. -I..\collection -I..\deps\ggml\include -II:/llama.cpp/include -II:/llama.cpp/ggml/include -DGGML_SHARED -DPOGLS_NO_VULKAN -c llama_pogls_runner_sid_v2.c -o llama_pogls_runner_sid_v2_new.o 2>build_err.txt
echo EXIT=%ERRORLEVEL%
type build_err.txt
