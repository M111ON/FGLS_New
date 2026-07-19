# FGLS Universal Codec — Build System
# Usage:
#   make              — build all binaries (fgls.exe, fgls_framed.exe)
#   make test         — build + run all tests
#   make clean        — remove build artifacts
#
# Compatible with: MSYS2 gcc (Windows), WSL gcc (Ubuntu), Linux gcc
# Override CC to use a specific compiler: make CC=/c/msys64/mingw64/bin/gcc.exe

CC      ?= gcc
CFLAGS  = -O2 -std=c11 -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -lm
INCLUDES = -Icore -Icollection -Irunner -Icollection/dgls/geo/include -Icollection/dgls/geo/frustum -Icollection/dgls/geo/Metatron -Icollection/core/core -Icollection/rdh -Icollection/active_updates -Icollection/dgls/bond/include
ZSTD_CFLAGS  = -DFGLS_USE_ZSTD
ZSTD_LDFLAGS = -lzstd
GEO_SRC = collection/dgls/geo/src/geo_jump.c

BIN = fgls.exe fgls_framed.exe verify_frame_seek.exe profile_test.exe tensor_proof.exe lblock_prototype.exe lblock_realdata_bench.exe atomic_reshape_demo.exe

all: $(BIN)

# Atomic reshape — separate TU (header-chain + static global state)
atomic_reshape_demo.exe: pipeline/atomic_reshape_cmd.c pipeline/atomic_reshape_standalone.c
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/atomic_reshape_cmd.c pipeline/atomic_reshape_standalone.c -o atomic_reshape_demo.exe $(LDFLAGS)

# fgls.exe links atomic_reshape_cmd.o so reshape-demo works
fgls.exe: pipeline/fgls_cli.c pipeline/atomic_reshape_cmd.c pipeline/timetravel_cmd.c collection/fgls_profile.h
	$(CC) $(CFLAGS) $(INCLUDES) $(ZSTD_CFLAGS) pipeline/fgls_cli.c pipeline/atomic_reshape_cmd.c pipeline/timetravel_cmd.c $(GEO_SRC) $(LDFLAGS) $(ZSTD_LDFLAGS) -o fgls.exe

nozstd: pipeline/fgls_cli.c collection/fgls_profile.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/fgls_cli.c $(GEO_SRC) $(LDFLAGS) -o fgls.exe

profile_test.exe: collection/fgls_profile_test.c collection/fgls_profile.h
	$(CC) $(CFLAGS) -Icollection collection/fgls_profile_test.c -o profile_test.exe

# FRAMED codec — geo_frame_seek + temporal delta (768B frames)
fgls_framed.exe: pipeline/fgls_framed.c collection/fgls_profile.h core/geo_frame_seek.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/fgls_framed.c -o fgls_framed.exe

# Verify geo_frame_seek invariants + stride-37 walk coverage
verify_frame_seek.exe: pipeline/verify_frame_seek.c core/geo_frame_seek.h
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/verify_frame_seek.c -o verify_frame_seek.exe

tensor_proof.exe: pipeline/tensor_proof.c collection/sid.h runner/addr_space.h
	$(CC) $(CFLAGS) -DGEO_JUMP_INLINE $(INCLUDES) -Icollection/dgls/geo/include -Icollection/rdh pipeline/tensor_proof.c -o tensor_proof.exe

lblock_prototype.exe: pipeline/lblock_prototype.c
	$(CC) $(CFLAGS) $(INCLUDES) pipeline/lblock_prototype.c -o lblock_prototype.exe -lm

lblock_realdata_bench.exe: pipeline/lblock_realdata_bench.c
	$(CC) $(CFLAGS) $(INCLUDES) -Wno-unused-result pipeline/lblock_realdata_bench.c -o lblock_realdata_bench.exe -lm

# Cross-platform byte compare (fc on Windows, cmp on Unix)
CMP = cmp
ifdef COMSPEC
	CMP = fc /b
endif

# Cross-platform python (python3 on Unix, python on Windows)
# Override with: make PYTHON=python3 test
PYTHON ?= $(shell command -v python3 >/dev/null 2>&1 && echo python3 || echo python)

# Full test suite
test: $(BIN)
	@echo "=== 1. Profile test ==="
	@./profile_test.exe
	@echo "=== 2. Version ==="
	@./fgls.exe version
	@echo "=== 3. Roundtrip zeros ==="
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\x00'*1024)"
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
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\xAA\\xBB\\xCC\\xDD'*1024)"
	@./fgls.exe encode pipeline/_t.bin pipeline/_t.gfuf > /dev/null
	@./fgls.exe decode pipeline/_t.gfuf pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.gfuf pipeline/_t_dec.bin
	@echo "=== 7. geo_frame_seek verify ==="
	@./verify_frame_seek.exe 2>&1 | tail -12
	@echo "=== 8. FRAMED roundtrip zeros ==="
	@$(PYTHON) -c "open('pipeline/_t.bin','wb').write(b'\\x00'*1024)"
	@./fgls_framed.exe encode pipeline/_t.bin pipeline/_t.frmd > /dev/null
	@./fgls_framed.exe decode pipeline/_t.frmd pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.frmd pipeline/_t_dec.bin
	@echo "=== 9. FRAMED roundtrip random 1MB (worst case, no crash) ==="
	@$(PYTHON) -c "import os; open('pipeline/_t.bin','wb').write(os.urandom(1024*1024))"
	@./fgls_framed.exe encode pipeline/_t.bin pipeline/_t.frmd > /dev/null
	@./fgls_framed.exe decode pipeline/_t.frmd pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  PASS" || echo "  FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.frmd pipeline/_t_dec.bin
	@echo "=== 10. FRAMED vs fgls_cli integration (1MB identical frames) ==="
	@$(PYTHON) -c "f=bytes(range(256))*3; open('pipeline/_t.bin','wb').write(f*1366)"
	@echo "  fgls_framed.exe encode:"
	@./fgls_framed.exe encode pipeline/_t.bin pipeline/_t.frmd 2>&1 | grep -E 'Output|reduction'
	@echo "  fgls.exe encode-framed:"
	@./fgls.exe encode-framed pipeline/_t.bin pipeline/_t.frmd2 2>&1 | grep -E 'Output|reduction'
	@$(CMP) pipeline/_t.frmd pipeline/_t.frmd2 > /dev/null 2>&1 && echo "  (identical output: PASS)" || echo "  (identical output: FAIL)"
	@rm -f pipeline/_t.bin pipeline/_t.frmd pipeline/_t.frmd2
	@echo "=== 11. fgls.exe encode-auto picks FRAMED on coherent data ==="
	@$(PYTHON) -c "f=bytes(range(256))*3; open('pipeline/_t.bin','wb').write(f*1366)"
	@./fgls.exe encode-auto pipeline/_t.bin pipeline/_t.auto 2>&1 | grep -E '→'
	@rm -f pipeline/_t.bin pipeline/_t.auto
	@echo "=== 12. Tensor proof (28 tests) ==="
	@./tensor_proof.exe 2>&1 | grep -E 'RESULTS|passed|failed'
	@echo "=== 13. L-block (FrustumBlock 4896B) roundtrip ==="
	@echo -n 'Hello, FGLS L-block test!' > pipeline/_t.bin
	@$(PYTHON) -c "open('pipeline/_t.bin','ab').write(b'Hello, FGLS L-block test!' * 100)"
	@./fgls.exe encode-lblock pipeline/_t.bin pipeline/_t.lblk 2>&1 | grep -E 'Input|Output|Blocks'
	@./fgls.exe decode-lblock pipeline/_t.lblk pipeline/_t_dec.bin > /dev/null
	@$(CMP) pipeline/_t.bin pipeline/_t_dec.bin > /dev/null 2>&1 && echo "  ROUNDTRIP: PASS" || echo "  ROUNDTRIP: FAIL"
	@rm -f pipeline/_t.bin pipeline/_t.lblk pipeline/_t_dec.bin
	@echo "=== 14. L-block prototype (random data reshape into Hilbert grid) ==="
	@./lblock_prototype.exe 2>&1 | grep -E 'RESULTS|passed|failed'
	@echo "=== 15. L-block realdata bench (64MB file) ==="
	@./lblock_realdata_bench.exe 2>&1 | grep -E 'BENCH COMPLETE|RAW random|L-block indexed|SID.*throughput|lookup'
	@echo "=== 16. fgls lblock-reshape (bond edges) ==="
	@rm -f pipeline/_t.bin pipeline/_t.lblk
	@echo -n 'FGLS lblock-reshape test ' > pipeline/_t.bin
	@python3 -c "open('pipeline/_t.bin','ab').write(b'0123456789ABCDEF'*20)"
	@./fgls.exe lblock-reshape pipeline/_t.bin pipeline/_t.lblk 2>&1 | grep -E 'L-block reshape|Edges'
	@rm -f pipeline/_t.bin pipeline/_t.lblk
	@echo "=== 17. fgls field-info (GeoField) ==="
	@rm -f pipeline/_t.bin pipeline/_t.field
	@echo -n 'FGLS field-info test ' > pipeline/_t.bin
	@python3 -c "open('pipeline/_t.bin','ab').write(b'0123456789ABCDEF'*32)"
	@./fgls.exe field-info pipeline/_t.bin pipeline/_t.field 2>&1 | grep -E 'Field info|GP level|Face max|Blocks|Chunks'
	@rm -f pipeline/_t.bin pipeline/_t.field
	@echo "=== 18. fgls tring-demo (Tring 64B timeline) ==="
	@./fgls.exe tring-demo pipeline/_t.tring 2>&1 | grep -E 'Tring demo|Roundtrip'
	@rm -f pipeline/_t.tring
	@echo "=== 19. fgls torus-demo (Dodeca Torus walk) ==="
	@./fgls.exe torus-demo pipeline/_t.torus 2>&1 | grep -E 'Torus demo|xray@'
	@rm -f pipeline/_t.torus
	@echo "=== 20. fgls reshape-demo (Atomic Reshape separate TU) ==="
	@./fgls.exe reshape-demo pipeline/_t.arshp 2>&1 | grep -E 'Atomic reshape|RESULTS|passed|failed|Wrote'
	@rm -f pipeline/_t.arshp
	@echo "=== 21. fgls dual-demo (Dual-world 162↔64 placement) ==="
	@./fgls.exe dual-demo pipeline/_t.dual 2>&1 | grep -E 'Dual-place|Verify|Roundtrip|Sum'
	@rm -f pipeline/_t.dual
	@echo "=== 22. fgls timetravel-demo (Rewind + Wang + temporal ring) ==="
	@./fgls.exe timetravel-demo pipeline/_t.tt 2>&1 | grep -E 'Timetravel|RESULTS|Stored|Rows|Tring|Recover|Pass|Wrote'
	@rm -f pipeline/_t.tt /tmp/timetravel_t5.bin
	@echo "=== ALL TESTS COMPLETE ==="

clean:
	@rm -f fgls.exe fgls_framed.exe verify_frame_seek.exe profile_test.exe tensor_proof.exe pipeline/_t.*

.PHONY: all test clean nozstd