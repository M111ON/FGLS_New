# DGLS Makefile
CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
INCLUDES = -Igeo/include -Igeo/frustum -Igeo/src -Idiamond/include -Idiamond/hamburger -Idiamond/gpx -Idiamond/hbv_bundle -Ibond/include -Itools
# zstd path for MinGW-W64 (adjust if using MSYS2)
ZLIB_PATH = -LC:/msys64/mingw64/lib

SRC_GEO   = geo/src/geo_jump.c
SRC_BOND  = bond/src/pogls_bond_export.c
OBJ       = geo_jump.o pogls_bond_export.o
TEST_SRC  = test_pipeline.c
BENCH_SRC = bench_dram_real.c
POC_SRC   = poc_dram_tile.c
GPX5_SRC  = tools/gpx5.c

.PHONY: all clean test benchmark poc gpx5-tool

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

benchmark: bench_dram_real.exe
	./bench_dram_real.exe

poc: poc_dram_tile.exe
	./poc_dram_tile.exe

clean:
	$(RM) *.o *.exe 2>/dev/null || del /Q *.o *.exe 2>nul || true
	$(RM) tools/gpx5.exe 2>/dev/null || del /Q tools\gpx5.exe 2>nul || true
