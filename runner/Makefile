CC      = gcc
CFLAGS  = -O2 -std=c11
CFLAGS += -I. -I../collection -I../collection/src -I../collection/core
CFLAGS += -I../collection/core/core -I../collection/geopixel
CFLAGS += -I../collection/geopixel/Metatron/core -I../collection/pogls_engine
CFLAGS += -II:/llama.cpp/include -II:/llama.cpp/ggml/include
LDLIBS  = -lm
DLLDIR  = I:/llama/llama-b9528-bin-win-vulkan-x64
DLLS    = $(DLLDIR)/llama.dll $(DLLDIR)/ggml.dll $(DLLDIR)/ggml-base.dll $(DLLDIR)/ggml-cpu-x64.dll

.PHONY: all clean run

all: llama_pogls_runner_sid.exe

llama_pogls_runner_sid.exe: llama_pogls_runner_sid.c gguf_index.h sid_loader.h sid_cache.h
	$(CC) $(CFLAGS) -o $@ $< $(DLLS) $(LDLIBS)

clean:
	rm -f *.exe tests/*.exe

run: all
	.\llama_pogls_runner_sid.exe --help
