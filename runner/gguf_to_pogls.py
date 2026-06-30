#!/usr/bin/env python3
"""GGUF → POGLS flat tensor store converter.

Usage: python gguf_to_pogls.py model.gguf output.pogls [filter]

Writes PoglsStore binary (matches C struct in pogls_store.h):
  Header: magic(u32) + version(u32) + n_tensors(u32) + flags(u32) + pad(48) = 64B
  Index:  20736 x {offset(u64) + nbytes(u32) + _pad(u32)} = 331776B
  Data:   tensor raw data packed sequentially
"""

import sys, struct, os
import gguf

POGLS_MAGIC = 0x53474F50  # "POGS" — matches C struct
POGLS_MAX_ADDR = 20736
POGLS_VERSION = 1

def name_to_addr(name: str) -> int:
    h = 2166136261
    for c in name.encode():
        h = (h ^ c) * 16777619 & 0xFFFFFFFF
    return (h % 12) * 1728 + ((h >> 8) % 12) * 144 + ((h >> 16) % 12) * 12 + ((h >> 24) % 12)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} model.gguf output.pogls [filter]"); sys.exit(1)
    model_path = sys.argv[1]; out_path = sys.argv[2]
    filter_str = sys.argv[3] if len(sys.argv) > 3 else "all"

    reader = gguf.GGUFReader(model_path)
    tensors = reader.tensors

    addr_map = {}
    n_kept = n_skip = n_collision = 0
    for t in tensors:
        name = t.name
        if filter_str != "all" and filter_str not in name:
            n_skip += 1; continue
        if (not name.startswith("blk.") and filter_str == "all"
            and name not in ("token_embd.weight","output_norm.weight","output.weight")):
            n_skip += 1; continue
        data = t.data
        sz = data.nbytes if hasattr(data, 'nbytes') else 0
        if sz == 0: n_skip += 1; continue
        addr = name_to_addr(name)
        if addr >= POGLS_MAX_ADDR: n_skip += 1; continue
        if addr in addr_map:
            n_collision += 1; continue
        raw = data.tobytes() if hasattr(data, 'tobytes') else bytes(data)
        addr_map[addr] = (name, sz, raw)
        n_kept += 1
        if n_kept % 50 == 0:
            print(f"  [{n_kept}] {name[:50]} -> addr={addr}", flush=True)

    print(f"[pogls] kept={n_kept} skip={n_skip} collision={n_collision}", flush=True)

    # Build index: each entry is Q(u64 offset) + I(u32 nbytes) + I(u32 _pad) = 16B
    # (total_sz below means the offset where data starts — NOT a header field)
    index = bytearray(POGLS_MAX_ADDR * 16)
    data_off = 64 + POGLS_MAX_ADDR * 16  # first byte after header+index
    for addr in sorted(addr_map):
        _, sz, _ = addr_map[addr]
        struct.pack_into('<QII', index, addr * 16, data_off, sz, 0)
        data_off += sz

    # Write
    with open(out_path, 'wb') as f:
        # Header: IIII + 48B pad = 64B
        f.write(struct.pack('<IIII', POGLS_MAGIC, POGLS_VERSION, len(addr_map), 0))
        f.write(b'\x00' * 48)
        # Index
        f.write(bytes(index))
        # Data
        for addr in sorted(addr_map):
            _, _, raw = addr_map[addr]
            f.write(raw)

    file_sz = os.path.getsize(out_path)
    print(f"[pogls] wrote {out_path}: {file_sz} bytes ({file_sz/1024/1024:.1f} MB)", flush=True)

    # Verify: C struct memory = same as on-disk (packed)
    v_ok = v_fail = 0
    with open(out_path, 'rb') as f:
        hdr = f.read(64)
        magic, ver, n_stored, flags = struct.unpack_from('<IIII', hdr, 0)
        assert magic == POGLS_MAGIC, f"bad magic {magic:#x} vs expected {POGLS_MAGIC:#x}"
        for addr, (name, sz, raw) in addr_map.items():
            f.seek(64 + addr * 16)
            off, n, _ = struct.unpack_from('<QII', f.read(16))
            if n != sz:
                v_fail += 1; print(f"  SIZE MISMATCH addr={addr} {name}: stored={n} actual={sz}", flush=True)
                continue
            f.seek(off)
            rb = f.read(n)
            if rb == raw: v_ok += 1
            else: v_fail += 1; print(f"  DATA FAIL addr={addr} {name}", flush=True)

    print(f"[pogls] verify: {v_ok} OK, {v_fail} FAIL", flush=True)
    return 0 if v_fail == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
