# FGLS Universal Codec — Build System
# Usage:
#   make              — build all binaries
#   make test         — build + run all tests
#   make clean        — remove build artifacts
#   make nozstd       — build fgls.exe without zstd
#
# Compiler: auto-detects working gcc. MSYS2 gcc 16.1.0 is broken (exit 1).
# Override: make CC=/path/to/gcc

# ── Compiler: detect broken MSYS2 gcc, fallback to MinGW ──
# mingw32-make default CC=cc doesn't exist on MSYS. Force override.
# Default: gcc. Override with: make CC=/path/to/gcc
CC ?= gcc
# Note: mingw32-make defaults CC=cc (broken on MSYS). Always pass CC explicitly:
#   make CC=gcc           (if gcc is in PATH)
#   make CC=/c/mingw64/bin/gcc.exe   (MinGW fallback)

CFLAGS  = -O2 -std=c11 -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -lm

# ── Include paths ──
INCLUDES = -Icore -Icollection -Irunner \
  -Icollection/dgls/geo/include -Icollection/dgls/geo/frustum \
  -Icollection/dgls/geo/Metatron -Icollection/core/core \
  -Icollection/rdh -Icollection/active_updates -Icollection/dgls/bond/include \
  -Icollection/dgls/diamond/include -Icollection/dgls/diamond/hamburger \
  -Icollection/dgls/diamond/hbv_bundle -Icollection/dgls/diamond/gpx
# ── Zstd: auto-detect when header + library available ──
# MSYS2 gcc 16.1.0 supports __has_include → auto-detects zstd.h.
# If found, link -lzstd so the undefined references don't break the build.
_ZSTD_H := $(or $(wildcard /c/msys64/mingw64/include/zstd.h),\
                $(wildcard C:/msys64/mingw64/include/zstd.h))
ifneq ($(_ZSTD_H),)
  ZSTD_CFLAGS  = -DFGLS_USE_ZSTD -IC:/msys64/mingw64/include
  ZSTD_LDFLAGS = -LC:/msys64/mingw64/lib -lzstd
else
  ZSTD_CFLAGS  =
  ZSTD_LDFLAGS =
endif
# Override: make ZSTD=0 to force no zstd even if header exists
ifdef ZSTD
  ifeq ($(ZSTD),0)
    ZSTD_CFLAGS  =
    ZSTD_LDFLAGS =
  endif
endif

GEO_SRC = collection/dgls/geo/src/geo_jump.c

BIN = fgls.exe verify_frame_seek.exe profile_test.exe tensor_proof.exe \
      lblock_prototype.exe lblock_realdata_bench.exe atomic_reshape_demo.exe \
      test_fibo_tick.exe test_enclosure_pipeline.exe test_tensor_track.exe \
      test_beam_entropy_container.exe

all: $(BIN)
	@echo "Built: $(BIN)"
	@echo "Compiler: $(CC)"
ifneq ($(ZSTD_LDFLAGS),)
	@echo "Zstd: yes"
else
	@echo "Zstd: no (install MSYS2 zstd for full support)"
endif

# ── Main binary ──
fgls.exe: pipeline/fgls_cli.c pipeline/atomic_reshape_cmd.c pipeline/timetravel_cmd.c pipeline/tensor_cmd.c collection/fgls_profile.h
	$(CC) $(CFLAGS) $(INCLUDES) $(ZSTD_CFLAGS) \
	  pipeline/fgls_cli.c pipeline/atomic_reshape_cmd.c \
	  pipeline/timetravel_cmd.c pipeline/tensor_cmd.c $(GEO_SRC) \
	  $(LDFLAGS) $(ZSTD_LDFLAGS) -o fgls.exe

nozstd: pipeline/fgls_cli.c collection/fgls_profile.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/fgls_cli.c $(GEO_SRC) $(LDFLAGS) -o fgls.exe

# ── Tools ──
atomic_reshape_demo.exe: pipeline/atomic_reshape_cmd.c pipeline/atomic_reshape_standalone.c
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/atomic_reshape_cmd.c pipeline/atomic_reshape_standalone.c -o atomic_reshape_demo.exe $(LDFLAGS)

profile_test.exe: collection/fgls_profile_test.c collection/fgls_profile.h
	$(CC) $(CFLAGS) -Icollection collection/fgls_profile_test.c -o profile_test.exe

verify_frame_seek.exe: pipeline/verify_frame_seek.c core/geo_frame_seek.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/verify_frame_seek.c -o verify_frame_seek.exe

tensor_proof.exe: pipeline/tensor_proof.c collection/sid.h runner/addr_space.h
	$(CC) $(CFLAGS) -DGEO_JUMP_INLINE $(INCLUDES) -Icollection/dgls/geo/include -Icollection/rdh pipeline/tensor_proof.c -o tensor_proof.exe

test_fibo_tick.exe: pipeline/test_fibo_tick.c core/fibo_tick.h core/geo_frame_seek.h collection/rdh/rdh_capture.h collection/include/p5h_ribcage.h collection/dgls/geo/include/fibo_spine.h
	$(CC) $(CFLAGS) $(INCLUDES) -DP5H_ENABLE pipeline/test_fibo_tick.c -o test_fibo_tick.exe

test_enclosure_pipeline.exe: pipeline/test_enclosure_pipeline.c collection/dgls/geo/include/gls_enclosure.h core/fibo_tick.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/test_enclosure_pipeline.c -o test_enclosure_pipeline.exe

test_tensor_track.exe: pipeline/test_tensor_track.c core/tensor_track.h core/fibo_tick.h collection/dgls/geo/include/gls_enclosure.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/test_tensor_track.c -o test_tensor_track.exe

test_beam_entropy_container.exe: pipeline/test_beam_entropy_container.c core/beam_entropy_container.h core/fibo_tick.h core/geo_frame_seek.h beam_addressing/beam_timer.h collection/rdh/rdh_capture.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/test_beam_entropy_container.c -o test_beam_entropy_container.exe -lm

lblock_prototype.exe: pipeline/lblock_prototype.c
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/lblock_prototype.c -o lblock_prototype.exe -lm

lblock_realdata_bench.exe: pipeline/lblock_realdata_bench.c
	$(CC) $(CFLAGS) $(INCLUDES) -Wno-unused-result pipeline/lblock_realdata_bench.c -o lblock_realdata_bench.exe -lm

# ── Cross-platform helpers ──
CMP = cmp
ifdef COMSPEC
  CMP = fc /b
endif
PYTHON ?= python

# ── Test suite ──
test: $(BIN)
	@echo "=== 1. Profile test ==="
	@./profile_test.exe
	@echo "=== 2. Version ==="
	@./fgls.exe version
	@echo "=== 3. Roundtrip zeros ==="
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\\\x00'*1024)"
	@./fgls.exe encode pipeline/_t.bin pipeline/_t.gfuf > /dev/null
	@./fgls.exe decode pipeline/_t.gfuf pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.gfuf pipeline/_t_dec.bin
	@echo "=== 4. Roundtrip sparse ==="
	@$(PYTHON) -c "d=bytearray(8192); [d.__setitem__(i*97%8192, i&255) for i in range(80)]; open('pipeline/_t.bin','wb').write(d)"
	@./fgls.exe encode pipeline/_t.bin pipeline/_t.gfuf > /dev/null
	@./fgls.exe decode pipeline/_t.gfuf pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.gfuf pipeline/_t_dec.bin
	@echo "=== 5. Roundtrip source ==="
	@./fgls.exe encode pipeline/fgls_cli.c pipeline/_t.gfuf > /dev/null
	@./fgls.exe decode pipeline/_t.gfuf pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/fgls_cli.c pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.gfuf pipeline/_t_dec.bin
	@echo "=== 6. Roundtrip repeated ==="
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\\\xAA\\\\xBB\\\\xCC\\\\xDD'*1024)"
	@./fgls.exe encode pipeline/_t.bin pipeline/_t.gfuf > /dev/null
	@./fgls.exe decode pipeline/_t.gfuf pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.gfuf pipeline/_t_dec.bin
	@echo "=== 7. geo_frame_seek verify ==="
	@./verify_frame_seek.exe 2>&1 | tail -12
	@echo "=== 8. FRMD roundtrip zeros ==="
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\\\x00'*1024)"
	@./fgls.exe encode-framed pipeline/_t.bin pipeline/_t.frmd > /dev/null
	@./fgls.exe decode-framed pipeline/_t.frmd pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.frmd pipeline/_t_dec.bin
	@echo "=== 9. FRMD roundtrip random 1MB ==="
	@$(PYTHON) -c "import os; open('pipeline/_t.bin','wb').write(os.urandom(1024*1024))"
	@./fgls.exe encode-framed pipeline/_t.bin pipeline/_t.frmd > /dev/null
	@./fgls.exe decode-framed pipeline/_t.frmd pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.frmd pipeline/_t_dec.bin
	@echo "=== 10. Tensor proof ==="
	@./tensor_proof.exe 2>&1 | grep -E 'RESULTS|passed|failed'
	@echo "=== 11. L-block roundtrip ==="
	@echo -n 'Hello, FGLS L-block test!' > pipeline/_t.bin
	@$(PYTHON) -c "open('pipeline/_t.bin','ab').write(b'Hello, FGLS L-block test!' * 100)"
	@./fgls.exe encode-lblock pipeline/_t.bin pipeline/_t.lblk 2>&1 | grep -E 'Input|Output|Blocks'
	@./fgls.exe decode-lblock pipeline/_t.lblk pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  ROUNDTRIP: PASS" || echo "  ROUNDTRIP: FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.lblk pipeline/_t_dec.bin
	@echo "=== 12. L-block prototype ==="
	@./lblock_prototype.exe 2>&1 | grep -E 'RESULTS|passed|failed'
	@echo "=== 13. L-block realdata bench ==="
	@./lblock_realdata_bench.exe 2>&1 | grep -E 'BENCH COMPLETE|RAW|L-block|throughput'
	@echo "=== 14. reshape-demo ==="
	@./fgls.exe reshape-demo pipeline/_t.arshp 2>&1 | grep -E 'Atomic|RESULTS|passed|failed|Wrote'
	@rm -f pipeline/_t.arshp
	@echo "=== 15. tring-demo ==="
	@./fgls.exe tring-demo pipeline/_t.tring 2>&1 | grep -E 'Tring|Roundtrip'
	@rm -f pipeline/_t.tring
	@echo "=== 16. torus-demo ==="
	@./fgls.exe torus-demo pipeline/_t.torus 2>&1 | grep -E 'Torus|xray@'
	@rm -f pipeline/_t.torus
	@echo "=== 17. timetravel-demo ==="
	@./fgls.exe timetravel-demo pipeline/_t.tt 2>&1 | grep -E 'Timetravel|RESULTS|Stored|Pass|Wrote'
	@rm -f pipeline/_t.tt pipeline/_timetravel_t5.bin
	@echo "=== 18. fibo_tick integration ==="
	@./test_fibo_tick.exe 2>&1 | grep -E 'PASS|FAIL|RESULTS'
	@echo "=== 19. enclosure pipeline ==="
	@./test_enclosure_pipeline.exe 2>&1 | grep -E 'PASS|FAIL|RESULTS'
	@echo "=== 20. tensor track ==="
	@./test_tensor_track.exe 2>&1 | grep -E 'PASS|FAIL|RESULTS'
	@echo "=== 21. beam entropy container ==="
	@./test_beam_entropy_container.exe 2>&1 | grep -E 'PASS|FAIL|RESULTS'
	@echo "=== ALL TESTS COMPLETE ==="

clean:
	@rm -f fgls.exe verify_frame_seek.exe profile_test.exe tensor_proof.exe \
	  lblock_prototype.exe lblock_realdata_bench.exe atomic_reshape_demo.exe \
	  test_fibo_tick.exe test_enclosure_pipeline.exe test_tensor_track.exe \
	  test_beam_entropy_container.exe fgls_framed.exe pipeline/_t.*

.PHONY: all test clean nozstd
