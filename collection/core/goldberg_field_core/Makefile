CC := gcc
CFLAGS := -O2 -Wall -I. -I../asset_headers -I../asset_headers/twin_core -I../pogls_engine -I../pogls_engine/core

NVCC := nvcc
NVCCFLAGS := -O2 -arch=sm_61

TESTS := test_tracker test_goldberg_pipeline test_tring_goldberg
BENCH := bench_gbring

.PHONY: all clean run_tests

all: $(TESTS) $(BENCH)

test_tracker: test_tracker.c
	$(CC) $(CFLAGS) $< -o $@

test_goldberg_pipeline: test_goldberg_pipeline.c
	$(CC) $(CFLAGS) $< -o $@

test_tring_goldberg: test_tring_goldberg.c
	$(CC) $(CFLAGS) $< -o $@

bench_gbring: geo_kernel_gbring.cu
	$(NVCC) $(NVCCFLAGS) $< -o $@

run_tests: $(TESTS)
	./test_tracker
	./test_goldberg_pipeline
	./test_tring_goldberg

clean:
	rm -f $(TESTS) $(BENCH)
