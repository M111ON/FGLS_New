import os, time, sys
sys.path.insert(0, '.')
from tools.geopixel_pipeline import *

# Test with actual model weights
model_path = r'I:\model\LFM2.5-8B-A1B-Q4_K_M.gguf'
if os.path.exists(model_path):
    size_mb = os.path.getsize(model_path) / 1024 / 1024
    print("Model: %.1f MB" % size_mb)
    with open(model_path, 'rb') as f:
        data = f.read(1024*1024)
    print("Sample: %d bytes" % len(data))
    t0 = time.perf_counter()
    result = pipeline_encode(data)
    t_enc = time.perf_counter() - t0
    t0 = time.perf_counter()
    dec = pipeline_decode(result.encoded)
    t_dec = time.perf_counter() - t0
    ok = dec['data'][:len(data)] == data
    print("Ratio: %.2fx  enc: %.0fms  dec: %.0fms  roundtrip: %s" % (result.ratio, t_enc*1000, t_dec*1000, ok))
    print("Skeleton: %s" % dict(zip(SKEL_NAMES, result.skel_stats)))
    print("xxh64: 0x%016x" % dec['digest'])
    print("File size: %d bytes" % len(result.encoded))
else:
    print("Model not found")

print()
# Test runner EXE
exe_path = r'I:\FGLS_new\runner\llama_pogls_runner_sid_v2.exe'
if os.path.exists(exe_path):
    size = os.path.getsize(exe_path)
    with open(exe_path, 'rb') as f:
        data = f.read(min(size, 512*1024))
    result = pipeline_encode(data)
    dec = pipeline_decode(result.encoded)
    ok = dec['data'][:len(data)] == data
    print("Runner EXE (%dKB): ratio=%.2fx  roundtrip=%s" % (len(data)/1024, result.ratio, ok))
    print("Skeleton: %s" % dict(zip(SKEL_NAMES, result.skel_stats)))
else:
    print("EXE not found")

print()
# Test Python source files
for name in ['geopixel_pipeline.py', 'geopixel_gui.py']:
    fpath = r'I:\FGLS_new\tools' + os.sep + name
    if os.path.exists(fpath):
        with open(fpath, 'rb') as f:
            data = f.read()
        result = pipeline_encode(data)
        dec = pipeline_decode(result.encoded)
        ok = dec['data'][:len(data)] == data
        skel = dict(zip(SKEL_NAMES, result.skel_stats))
        total_comp = sum(len(skeleton_compress_chunk(data[i*64:(i+1)*64], cr['skel_strategy']) 
                        for i, cr in enumerate(result.coord_records)))
        print("%s (%dKB): ratio=%.2fx  roundtrip=%s  skel=%s" % (name, len(data)/1024, result.ratio, ok, skel))

print()
# Summary of GEOM subtype system
print("=== GEOM Evolution Status ===")
print("INLINE  (0x00): active, 65B baseline, correctness-first")
print("BLUEPRINT (0x01): reserved, spec at docs/geom-blueprint-spec.md")
print("PARAMETRIC (0x02): reserved, future")
print("TEMPLATE (0x03): reserved, future")
print()
print("Layer 1 (Spec): gp_chunk_to_addr, skel_decide, skeleton_lookup, Diamond Shell, frame, tring, wallet_seed")
print("Layer 2 (Storage): GPXL v2 (48B header, xxh64, CoordRecord section, GEOM subtypes)")
print("Layer 3 (Runtime): Drain, Shadow, Metatron, Fiber, ShapeRoute, MultiResolution - not yet implemented")
