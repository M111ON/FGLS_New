import sys
sys.path.insert(0, 'tools')
from geopixel_pipeline import (pipeline_encode, pipeline_decode, skeleton_decompress_chunk,
                               SKEL_ID, SKEL_FLAT, SKEL_DIFF, SKEL_BREF, SKEL_GEOM, SKEL_RAW,
                               SKEL_NAMES, CHUNK_SZ, struct, GPXL_MAGIC, GPXL_HEADER_SZ)

data = open('collection/wallet_seed_c.dll', 'rb').read()
result = pipeline_encode(data)

# Parse the encoded to find which chunks are problematic
encoded = result.encoded
offset = GPXL_HEADER_SZ
n_chunks = result.stats['n_chunks']
original_size = result.stats['original_size']

# Read coord records
crs = []
for i in range(n_chunks):
    face, edge, z, skel, seed_lo, seed_hi, chksum, fast_sig = struct.unpack_from('<BBBBQII', encoded, offset)
    crs.append({'face': face, 'edge': edge, 'z': z, 'skel': skel, 'seed': (seed_hi << 32) | seed_lo, 'chk': chksum})
    offset += 20

# Read compressed chunks
chunks_data = encoded[offset:]

# Now find which chunk is at byte 9664
chunk_idx = 9664 // CHUNK_SZ
print(f"Chunk {chunk_idx} (byte {chunk_idx * CHUNK_SZ}-{(chunk_idx+1)*CHUNK_SZ})")

# Decompress that chunk
pos = 0
for ci in range(n_chunks):
    skel = crs[ci]["skel"]
    if skel == SKEL_ID:
        chunk_sz = 1
    elif skel == SKEL_FLAT:
        chunk_sz = 2
    elif skel == SKEL_RAW:
        chunk_sz = 1 + CHUNK_SZ
    elif skel == SKEL_DIFF:
        chunk_sz = 1 + CHUNK_SZ  # worst case
    elif skel == SKEL_BREF:
        chunk_sz = 3  # marker + ref_idx(2)
    else:
        chunk_sz = 1 + CHUNK_SZ

    if ci == chunk_idx:
        blob = chunks_data[pos:pos+chunk_sz]
        dec_chunk = skeleton_decompress_chunk(blob, skel, ci, result.coord_records)
        orig_chunk = data[ci*CHUNK_SZ:(ci+1)*CHUNK_SZ]
        print(f"  skel={SKEL_NAMES[skel]}")
        print(f"  orig:  {orig_chunk[:16].hex()}")
        print(f"  dec:   {bytes(dec_chunk[:16]).hex()}")
        match = bytes(dec_chunk) == orig_chunk
        print(f"  match: {match}")
        if not match:
            for b in range(CHUNK_SZ):
                if dec_chunk[b] != orig_chunk[b]:
                    print(f"  first diff at byte {b}: orig=0x{orig_chunk[b]:02x} dec=0x{dec_chunk[b]:02x}")
                    break
        break

    pos += chunk_sz

# Show skeleton distribution
from collections import Counter
skel_counts = Counter(cr['skel'] for cr in crs)
print(f"\nSkeleton distribution:")
for sk, cnt in sorted(skel_counts.items()):
    print(f"  {SKEL_NAMES[sk]:>5}: {cnt:>5} ({cnt/n_chunks*100:.1f}%)")
