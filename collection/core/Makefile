# Makefile — S83 POGLS runner
# Usage: make LLAMA_DIR=/path/to/llama.cpp

LLAMA_DIR ?= I:/llama/llama_cuda124_x64
CC        := gcc
CFLAGS    := -O2 -Wall -I. -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
LDFLAGS   := -L$(LLAMA_DIR)/build -lllama -lggml -Wl,-rpath,$(LLAMA_DIR)/build

SRCS := llama_pogls_runner.c \
        llama_pogls_backend.c \
        pogls_recon_file.c \
        gguf_to_model_index.c

TARGET := pogls_runner

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

# preflight-only test (no llama linkage needed)
preflight_test: gguf_to_model_index.c pogls_recon_file.c
	$(CC) -O2 -I. -DGGUF_MIDX_TEST gguf_to_model_index.c -o gguf_midx_test

clean:
	rm -f $(TARGET) gguf_midx_test
