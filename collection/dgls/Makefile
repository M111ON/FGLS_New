# DGLS Makefile
CC = /c/msys64/mingw64/bin/gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
INCLUDES = -Igeo/include -Igeo/frustum -Igeo/src -Idiamond/include -Idiamond/hamburger -Idiamond/gpx -Idiamond/hbv_bundle -Ibond/include -Itools -I..
# Extra includes for integration test (DRamTile, Gear Lock, RDH, POGLS engine)
INTEG_INCLUDES = $(INCLUDES) -I../../runner -I../../src -I../../core/core -I../rdh
# zstd path for MinGW-W64 (adjust if using MSYS2)
ZLIB_PATH = -LC:/msys64/mingw64/lib

SRC_GEO   = geo/src/geo_jump.c
SRC_BOND  = bond/src/pogls_bond_export.c
OBJ       = geo_jump.o pogls_bond_export.o
TEST_SRC  = test_pipeline.c
BENCH_SRC = bench_dram_real.c
POC_SRC   = poc_dram_tile.c
GPX5_SRC  = tools/gpx5.c
INTEG_SRC = test_pipeline_integration.c
GGUF_SRC  = test_gguf_pipeline.c

.PHONY: all clean test benchmark poc gpx5-tool integ test-integ gguf-test

all: test_pipeline.exe

geo_jump.o: $(SRC_GEO)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

pogls_bond_export.o: $(SRC_BOND)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test_pipeline.exe: $(TEST_SRC) $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm $(ZLIB_PATH) -lzstd

bench_dram_real.exe: $(BENCH_SRC)
	$(CC) -O2 -std=c11 $< -o $@

poc_dram_tile.exe: $(POC_SRC)
	$(CC) -O2 -std=c11 $< -o $@

gpx5-tool: tools/gpx5.exe

tools/gpx5.exe: $(GPX5_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SRC_GEO) -o $@ $(ZLIB_PATH) -lzstd

test: all
	./test_pipeline.exe

integ: test_pipeline_integration.exe
	./test_pipeline_integration.exe

test-integ: all integ
	./test_pipeline.exe && ./test_pipeline_integration.exe

test_pipeline_integration.exe: $(INTEG_SRC) $(OBJ)
	$(CC) $(CFLAGS) $(INTEG_INCLUDES) -c $(INTEG_SRC) -o test_pipeline_integration.o
	$(CC) $(CFLAGS) $(INTEG_INCLUDES) test_pipeline_integration.o $(OBJ) -o $@ -lm $(ZLIB_PATH) -lzstd

test_gguf_pipeline.exe: $(GGUF_SRC) $(OBJ)
	$(CC) $(CFLAGS) $(INTEG_INCLUDES) -c $(GGUF_SRC) -o test_gguf_pipeline.o
	$(CC) $(CFLAGS) $(INTEG_INCLUDES) test_gguf_pipeline.o $(OBJ) -o $@ -lm $(ZLIB_PATH) -lzstd

gguf-test: test_gguf_pipeline.exe
	./test_gguf_pipeline.exe 'I:\model\Qwen3-0.6B-Q4_0.gguf' 1000
	./test_gguf_pipeline.exe 'I:\model\Kokoro_no_espeak_Q8.gguf' 1000

benchmark: bench_dram_real.exe
	./bench_dram_real.exe

poc: poc_dram_tile.exe
	./poc_dram_tile.exe

clean:
	$(RM) *.o *.exe 2>/dev/null || del /Q *.o *.exe 2>nul || true
	$(RM) tools/gpx5.exe 2>/dev/null || del /Q tools\gpx5.exe 2>nul || true
