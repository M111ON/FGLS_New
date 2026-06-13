"""
build_pentagon_lut.py — Build cross-shell pentagon anchor LUT from Q4 gate export
===================================================================================
Q4 geometry structure is quantization-invariant → use as seed for all shells

Output: pentagon_lut.bin
  HEADER: magic(8)="PENT_LUT" + n_shells(4LE) + n_zones(4LE) = 16B
  ENTRIES per shell: shell_id(2) + zone(1) + ns(1) + shape(1) + pad(3) = 8B each
  LUT: zone_from[12] + zone_to[12] = 24B per shell pair

Usage:
  python build_pentagon_lut.py --gate gate_export/ --store mymodel.gsidx
"""

import os, struct, sys
import numpy as np

# ── Config ──────────────────────────────────────────────────────
MAGIC        = b"PENT_LUT"
N_ZONES      = 12
STRIDE       = 37
WALK_LEN     = 1032
FACE_SZ      = WALK_LEN // N_ZONES   # 86

# ── Gate (same as proof script) ─────────────────────────────────
class Gate:
    def __init__(self, gate_dir):
        def load(name):
            return np.fromfile(os.path.join(gate_dir, name), dtype=np.float32)
        self.enc_w = load('encoder_0_weight.f32').reshape(256, 128)
        self.enc_b = load('encoder_0_bias.f32')
        self.ln_w  = load('encoder_1_weight.f32')
        self.ln_b  = load('encoder_1_bias.f32')
        self.out_w = load('encoder_3_weight.f32').reshape(32, 256)
        self.out_b = load('encoder_3_bias.f32')
        self.codes = load('codebook_codes.f32').reshape(1024, 32)

    def encode(self, x):
        h = x @ self.enc_w.T + self.enc_b
        mu, va = h.mean(1, keepdims=True), h.var(1, keepdims=True)
        h = (h - mu) / np.sqrt(np.maximum(va, 1e-10))
        h = h * self.ln_w + self.ln_b
        h = 0.5 * h * (1 + np.tanh(np.sqrt(2/np.pi) * (h + 0.044715 * h**3)))
        h = h @ self.out_w.T + self.out_b
        dist = np.sum((h[:, None] - self.codes[None])**2, axis=2)
        return dist.argmin(axis=1)

    def zone(self, idx):
        enc = (idx * STRIDE) % WALK_LEN
        return enc // FACE_SZ   # 0..11


# ── GSIDX reader (no dependency on geo_store_reader.h) ──────────
GSIDX_MAGIC   = b"GSIDX001"
HEADER_SZ     = 32
ENTRY_SZ      = 20
NS_NAMES      = {0:'_', 12:'Q', 24:'K', 36:'V', 48:'O', 60:'G', 72:'U', 84:'D'}
SHAPES        = "IOTSZL"

def read_gsidx(path):
    entries = []
    with open(path, 'rb') as f:
        hdr = f.read(HEADER_SZ)
        assert hdr[:8] == GSIDX_MAGIC, "bad magic"
        n = struct.unpack_from('<I', hdr, 8)[0]
        for _ in range(n):
            raw = f.read(ENTRY_SZ)
            rz, si = struct.unpack_from('<HH', raw, 0)
            off, nr, nc = struct.unpack_from('<QII', raw, 4)
            ns  = (rz // 12) * 12
            zone = rz - ns
            entries.append({'ns': ns, 'zone': zone, 'shape': SHAPES[si],
                            'raw_zone': rz, 'offset': off,
                            'n_rows': nr, 'n_cols': nc})
    return entries


# ── Build LUT ───────────────────────────────────────────────────
def build_lut(gate, entries, gsdat_path):
    """
    For each entry: load rows → pool128 → encode → get zone distribution
    Returns: list of {ns, zone, shape, zone_dist[12], anchor_zone}
    """
    dat = np.fromfile(gsdat_path, dtype=np.float32)
    lut = []

    for e in entries:
        n_floats = e['n_rows'] * e['n_cols']
        start    = e['offset'] // 4   # bytes → float index
        rows     = dat[start : start + n_floats].reshape(e['n_rows'], e['n_cols'])

        # pool to 128
        cols = min(rows.shape[1], 1024)
        p128 = rows[:, :cols].reshape(rows.shape[0], -1, 8).mean(axis=2) \
               if cols >= 128 else np.pad(rows, ((0,0),(0,128-cols)))

        idx   = gate.encode(p128)
        zones = gate.zone(idx)
        dist  = np.bincount(zones, minlength=N_ZONES)

        # anchor = dominant zone (pentagon face with most weight)
        anchor = int(dist.argmax())

        lut.append({
            'ns': e['ns'], 'zone': e['zone'], 'shape': e['shape'],
            'anchor': anchor, 'dist': dist.tolist()
        })
        print(f"  ns={NS_NAMES.get(e['ns'],'?')} zone={e['zone']} "
              f"shape={e['shape']} anchor={anchor} "
              f"dist={dist.tolist()}")

    return lut


# ── Serialize LUT → binary ───────────────────────────────────────
def write_lut(lut, out_path):
    """
    Format:
      HEADER: magic(8) + n_entries(4) + pad(4) = 16B
      ENTRY:  ns(1) + zone(1) + shape(1) + anchor(1) + dist[12](12×2B) = 28B
    """
    n = len(lut)
    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<II', n, N_ZONES))
        for e in lut:
            f.write(struct.pack('<BBBB',
                e['ns'] // 12,   # ns_id 0-7
                e['zone'],
                ord(e['shape']),
                e['anchor']))
            for d in e['dist']:
                f.write(struct.pack('<H', min(d, 65535)))
    print(f"\n[write] {out_path}  ({os.path.getsize(out_path)} bytes, {n} entries)")


# ── Main ─────────────────────────────────────────────────────────
def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--gate',  default='gate_export', help='gate export dir')
    p.add_argument('--store', default='model',       help='base path to .gsidx/.gsdat')
    p.add_argument('--out',   default='pentagon_lut.bin')
    args = p.parse_args()

    print(f"[gate] loading from {args.gate}/")
    gate = Gate(args.gate)

    idx_path = args.store if args.store.endswith('.gsidx') else args.store + '.gsidx'
    dat_path = idx_path.replace('.gsidx', '.gsdat')

    print(f"[store] reading {idx_path}")
    entries = read_gsidx(idx_path)
    print(f"  {len(entries)} entries")

    print(f"\n[build lut]")
    lut = build_lut(gate, entries, dat_path)

    write_lut(lut, args.out)
    print(f"\n[done] pentagon_lut.bin ready — use as seed for cross-shell routing")


if __name__ == '__main__':
    main()
