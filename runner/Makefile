# Makefile — SID Inference Runner
# Requires:
#   - llama-b9528-bin-win-vulkan-x64 DLLs (copy to C:\TPOGLS\ or runner dir)
#   - MinGW64 (gcc, msys2)
#   - libzstd (from msys2: pacman -S mingw-w64-x86_64-zstd)

CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDFLAGS  = -lm

# Paths
LLAMA_INC = I:/llama/llama_cuda124_x64/include
LLAMA_DLL = C:/TPOGLS
COLLECTION = ../collection

# DLLs to link against
LLAMA_LIBS = \
    $(LLAMA_DLL)/llama.dll \
    $(LLAMA_DLL)/ggml.dll \
    $(LLAMA_DLL)/ggml-base.dll \
    $(LLAMA_DLL)/ggml-cpu-x64.dll \
    -lzstd

INCLUDES = -I. -I$(LLAMA_INC) -I$(COLLECTION)

# Default target
all: runner tests

# Main runner
runner: llama_pogls_runner_sid.exe

llama_pogls_runner_sid.exe: llama_pogls_runner_sid.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ llama_pogls_runner_sid.c $(LLAMA_LIBS) $(LDFLAGS)

# Tests
tests: test_sid_loader.exe test_sid_cache.exe test_integration.exe bench_cache.exe

test_sid_loader.exe: tests/test_sid_loader.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_sid_loader.c $(LLAMA_LIBS) $(LDFLAGS)

test_sid_cache.exe: tests/test_sid_cache.c sid_cache.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_sid_cache.c $(LDFLAGS)

test_integration.exe: tests/test_integration.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_integration.c $(LLAMA_LIBS) $(LDFLAGS)

bench_cache.exe: tests/bench_cache.c sid_cache.h
	$(CC) $(CFLAGS) -I. -o $@ tests/bench_cache.c $(LDFLAGS)

# Phony targets
.PHONY: clean run run-chat

clean:
	rm -f *.exe tests/*.exe

# Quick run
run: runner
	./llama_pogls_runner_sid.exe "$(MODEL)" --prompt "$(PROMPT)" --max-new 20

run-chat: runner
	./llama_pogls_runner_sid.exe "$(MODEL)" --chat --max-new 256

# Example:
#   make run MODEL="I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf" PROMPT="What is 2+2?"
#   make run-chat MODEL="I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"
