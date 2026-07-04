CC      ?= gcc
# On Windows with multiple MinGW installations, set CC explicitly:
#   CC=C:/msys64/mingw64/bin/gcc
CFLAGS  = -O2 -std=c11 -Wall -Wextra -Wpedantic
INCS    = -Iinclude -Iinclude/geopixel -Iinclude/hamburger -Iinclude/hbv
INCS   += -Iinclude/bond -Iinclude/sync
LDLIBS  = -lm

# zstd: set ZSTD_HOME to your zstd installation root
#   Linux (apt): ZSTD_HOME=/usr  (default)
#   msys2:       ZSTD_HOME=C:/msys64/mingw64
#   vcpkg:       ZSTD_HOME=C:/vcpkg/installed/x64-windows
ifdef ZSTD_HOME
INCS   += -I$(ZSTD_HOME)/include
LDLIBS += -L$(ZSTD_HOME)/lib -lzstd
else
# try system path
LDLIBS += -lzstd
endif

# libpng: set PNG_HOME if not in system path
#   msys2: PNG_HOME=C:/msys64/mingw64
ifdef PNG_HOME
LDLIBS += -L$(PNG_HOME)/lib -lpng
else
LDLIBS += -lpng
endif

# zlib: required by hb_vault (hbv) for deflate/inflate
#   msys2: ZLIB_HOME=C:/msys64/mingw64
ifdef ZLIB_HOME
LDLIBS += -L$(ZLIB_HOME)/lib -lz
else
LDLIBS += -lz
endif

# pthread (Linux / msys2)
# LDLIBS += -lpthread

.PHONY: all tests demos clean

all: tests demos

## ── Source objects ──────────────────────────────────────────

GEOPIXEL_OBJ = src/geopixel_v21_o25.o

## ── Tests ───────────────────────────────────────────────────

TEST_BINS = \
	tests/test_hamburger \
	tests/test_hamburger2 \
	tests/test_tile_stream \
	tests/test_calibrate_bmp \
	tests/test_classify_thresh \
	tests/test_multicycle_hilbert \
	tests/test_multicycle_stream \
	tests/test_hbhf_wire \
	tests/test_header_frame \
	tests/test_frame_seek \
	tests/codec_tile_test \
	tests/gpxtool \
	tests/test_gear_proof \
	tests/test_pipeline_proof

tests: $(TEST_BINS)

tests/test_hamburger: tests/test_hamburger.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_hamburger2: tests/test_hamburger2.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_tile_stream: tests/test_tile_stream.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_calibrate_bmp: tests/test_calibrate_bmp.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_classify_thresh: tests/test_classify_thresh.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_multicycle_hilbert: tests/test_multicycle_hilbert.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_multicycle_stream: tests/test_multicycle_stream.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_hbhf_wire: tests/test_hbhf_wire.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_header_frame: tests/test_header_frame.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/test_frame_seek: tests/test_frame_seek.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LDLIBS)

tests/codec_tile_test: tests/codec_tile_test.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

tests/gpxtool: tests/gpxtool.c $(GEOPIXEL_OBJ)
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ $(LDLIBS)

## ── Demos ───────────────────────────────────────────────────

demos: demo/p5h_barrier

demo/p5h_barrier: demo/demo_p5h_barrier.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LDLIBS)

## ── Objects ─────────────────────────────────────────────────

src/geopixel_v21_o25.o: src/geopixel_v21_o25.c
	$(CC) $(CFLAGS) $(INCS) -DPOGLS_LIB -c -o $@ $<

## ── Run all tests ───────────────────────────────────────────

check: $(TEST_BINS)
	@echo "Running all geopixel tests..."
	@set -e; \
	for t in $(TEST_BINS); do \
		echo "  [$$t]"; \
		./$$t || exit 1; \
	done; \
	echo "All tests PASS"

tests/test_gear_proof: tests/test_gear_proof.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LDLIBS)

tests/test_pipeline_proof: tests/test_pipeline_proof.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LDLIBS) -lm

## ── Clean ───────────────────────────────────────────────────

clean:
	rm -f src/*.o $(TEST_BINS) demo/p5h_barrier
