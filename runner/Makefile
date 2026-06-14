CC      = gcc
CFLAGS  = -O2 -I. -IC:/Users/Administrator.AVENTADOR/.opencode/llama -I../collection
LDFLAGS = -LC:/Users/Administrator.AVENTADOR/.opencode/llama -lllama -lggml -lggml-base -lggml-cpu
LIBS    = -lm -lpthread -lole32 -ladvapi32

.PHONY: all clean test

all: llama_pogls_runner_sid.exe

llama_pogls_runner_sid.exe: llama_pogls_runner_sid.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f *.exe *.o

test: all
	./llama_pogls_runner_sid.exe
