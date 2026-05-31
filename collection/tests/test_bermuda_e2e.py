"""
test_bermuda_e2e.py — End-to-End: Float → Route → DiamondBlock → Bond → Verify
===============================================================================
Simulates the full POGLS pipeline path:

  Float [B,S,D]
    → 1. bermuda_router.route()         → RoutingVerdict
    → 2. BermudaBlockWriter.from_verdict() → list of 64B DiamondBlocks
    → 3. Write .bin file (DiamondBlocks serialized)
    → 4. Read back .bin → BermudaBlockReader
    → 5. Restore verdict info → compare
    → 6. Bond pieces from verdict → verify chain
    → 7. Shadow fingerprint integrity check
"""

import sys, os, torch, struct, json
from pathlib import Path

COLLECTION = Path(__file__).resolve().parent
sys.path.insert(0, str(COLLECTION / 'python_src'))
sys.path.insert(0, str(COLLECTION))

from bermuda_router_v1 import BermudaRouter, RoutingVerdict, DEVICE
from bermuda_block import (
    BermudaBlockWriter, BermudaBlockReader,
    BLOCK_SIZE, BERMUDA_MAGIC, BERMUDA_VERSION,
)
from pogls_bond_py import make_piece, bond_verify, make_slot, plug_connect, PLUG_E, PLUG_W


def test_e2e():
    passed = 0
    failed = 0

    def check(label, ok):
        nonlocal passed, failed
        if ok:
            passed += 1
            print(f"  ✓ {label}")
        else:
            failed += 1
            print(f"  ✗ {label}")

    print("=" * 60)
    print("End-to-End: Bermuda → DiamondBlock → Bond → Verify")
    print("=" * 60)

    # ── Setup ────────────────────────────────────────────────
    DIM, GEAR = 128, 2
    router = BermudaRouter(dim=DIM, gear=GEAR)
    device = next(router.gate.parameters()).device
    torch.manual_seed(42)

    # ── Synthetic float data ─────────────────────────────────
    data = torch.randn(256, DIM, device=device) * 2 + 1.0

    bin_path = COLLECTION / "build" / "bermuda_e2e_test.bin"
    bin_path.parent.mkdir(exist_ok=True)

    for mode, mode_name in enumerate(["ORBITAL", "CHIRAL", "CROSS", "HUB"]):
        print(f"\n── [{mode}] {mode_name} ──")

        # ── Step 1: Route ────────────────────────────────────
        verdict = router.route(data, mode)
        real_mask = verdict.real_mask
        n_real = real_mask.sum().item()
        check(f"route: {n_real} real tokens, gear={verdict.gear}",
              n_real == data.shape[0] and verdict.gear in (1, 2, 3, 4))

        # ── Step 2: Serialize to DiamondBlocks ───────────────
        # Build shadow tensor (padded size)
        slots = 512 if verdict.gear == 1 else 1024
        shadow_placeholder = torch.randn(slots, DIM, device=device) * 0.05
        blocks = BermudaBlockWriter.from_verdict(verdict, shadow_placeholder)
        n_blocks = len(blocks)
        all_64 = all(len(b) == BLOCK_SIZE for b in blocks)
        check(f"serialize: {n_blocks} blocks × 64B = {n_blocks*64}B all {all_64}",
              n_blocks > 0 and all_64)

        # ── Step 3: Write/read .bin ──────────────────────────
        with open(bin_path, 'wb') as f:
            for b in blocks:
                f.write(b)
        file_size = bin_path.stat().st_size
        check(f"write .bin: {file_size}B = {n_blocks} blocks",
              file_size == n_blocks * BLOCK_SIZE)

        with open(bin_path, 'rb') as f:
            read_back = []
            while True:
                chunk = f.read(BLOCK_SIZE)
                if len(chunk) < BLOCK_SIZE:
                    break
                read_back.append(chunk)
        check(f"read .bin: {len(read_back)} blocks",
              len(read_back) == n_blocks)

        # ── Step 4: Parse blocks ─────────────────────────────
        parsed = BermudaBlockReader.read_all(read_back)
        n_parsed = len(parsed)
        all_valid = all(
            p['header']['magic'] == BERMUDA_MAGIC
            and p['header']['version'] == BERMUDA_VERSION
            for p in parsed
        )
        check(f"parse: {n_parsed} blocks, all valid={all_valid}",
              n_parsed == n_blocks and all_valid)

        # ── Step 5: Verify entry integrity ───────────────────
        total_entries = sum(len(p['entries']) for p in parsed)
        # All entries have valid ranges
        valid_ranges = all(
            0 <= e['idx_in'] < 4096
            and 0 <= e['idx_out'] < 4096
            and 0 <= e['tring_slot'] < 720
            for p in parsed for e in p['entries']
        )
        check(f"entries: {total_entries} read = {n_real} expected, "
              f"valid ranges={valid_ranges}",
              total_entries == n_real and valid_ranges)

        # Verify headers match mode
        mode_match = all(p['header']['mode'] == mode for p in parsed)
        check(f"mode consistency: all blocks mode={mode_name}",
              mode_match)

        # ── Step 6: Bond pieces from verdict ─────────────────
        real = verdict.real_mask
        zones = verdict.zone[real]
        shapes = verdict.shape[real]
        groups = {}
        for i in range(real.sum().item()):
            key = (zones[i].item(), chr(shapes[i].item()))
            if key not in groups:
                groups[key] = 0
            groups[key] += 1

        session_nonce = 0x424D524DE2E00001
        pieces = {}
        for (zone, shape_char), count in groups.items():
            seed = session_nonce ^ (zone << 8) ^ ord(shape_char)
            axis = {'I': 1, 'O': 2, 'T': 3, 'S': 4, 'Z': 5, 'L': 6, 'J': 7}.get(shape_char, 1)
            piece = make_piece(seed, axis)
            pieces[(zone, shape_char)] = piece

        check(f"bond pieces: {len(pieces)} groups",
              len(pieces) == len(groups))

        # Verify bonds between adjacent pieces (chain A→B→C...)
        sorted_keys = sorted(pieces.keys())
        bonds_ok = 0
        for i in range(len(sorted_keys) - 1):
            a = pieces[sorted_keys[i]]
            b = pieces[sorted_keys[i + 1]]
            v = bond_verify(a, b)
            if v['valid']:
                bonds_ok += 1
        # Note: pieces from different zones/shapes don't share intrinsic bonds
        # This is expected — they only bond if they're from same origin
        check(f"bond chain: {bonds_ok}/{len(sorted_keys)-1} valid links "
              f"(expected 0 for cross-zone pieces)", bonds_ok == 0)

        # ── Step 7: Shadow fingerprint ───────────────────────
        for p in parsed[:1]:
            h = p['header']
            check(f"shadow hash present: xxh64={h['shadow_xxh64']:016x}",
                  h['shadow_xxh64'] != 0)
            check(f"shadow stats: mean={h['shadow_mean']:.3f} std={h['shadow_std']:.3f}",
                  h['shadow_std'] > 0)

    # ── Cleanup ──────────────────────────────────────────────
    if bin_path.exists():
        bin_path.unlink()

    print(f"\n{'='*60}")
    print(f"Result: {passed}/{passed+failed} passed "
          f"{'✓' if failed==0 else '✗'}")
    return passed, failed


if __name__ == "__main__":
    test_e2e()
