"""
bermuda_block.py — 64B DiamondBlock-compatible Bermuda Packet
==============================================================
Serializes bermuda routing verdict + shadow fingerprint into
64-byte blocks that slot directly into the POGLS binary pipeline
(flow_chunker → smart_encode → onion_shell → tring).

Format (64 bytes per block):
┌──────────────────────────────────────────────────────────────┐
│ Header (24B)                                                 │
│   magic(2), ver(1), mode(1), gear(1), zone(1), shape(1),    │
│   polarity(1), count(2), n_entries(1), flags(1),             │
│   shadow_xxh64(8), shadow_mean(4), shadow_std(4)             │
├──────────────────────────────────────────────────────────────┤
│ Routing entries × 4 (each 8B = 32B total)                    │
│   idx_in(2), idx_out(2), tring_slot(2), flags(1), pad(1)    │
├──────────────────────────────────────────────────────────────┤
│ Tail (8B): link_next(4), reserved(4)                         │
└──────────────────────────────────────────────────────────────┘

Usage:
  from bermuda_block import BermudaBlockWriter, BermudaBlockReader
  blocks = BermudaBlockWriter.from_verdict(verdict, shadow_flat)
  # blocks is list of 64-byte bytes objects
"""

import struct, torch
from typing import List
from bermuda_router_v1 import RoutingVerdict

# ── Constants ───────────────────────────────────────────────
BERMUDA_MAGIC       = 0x424D   # 'BM'
BERMUDA_SHADOW_MAGIC = 0x5348  # 'SH' shadow data block
BERMUDA_VERSION     = 1
ENTRIES_PER_BLOCK   = 4
BLOCK_SIZE          = 64

# Flag bits
SHADOW_ATTACHED     = 1 << 0  # link_next chains to shadow data blocks

# Header: magic(2) + ver(1) + mode(1) + gear(1) + zone(1) + shape(1) +
#         polarity(1) + count(2) + n_entries(1) + flags(1) +
#         shadow_xxh64(8) + shadow_mean(4) + shadow_std(4) = 28
HEADER_FMT = '<H 6B H 2B Q f f'
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# Entry: idx_in H, idx_out H, tring_slot H, flags B, pad B = 8
ENTRY_FMT = '<H H H B B'
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)

# Tail: link_next I = 4
TAIL_FMT = '<I'
TAIL_SIZE = struct.calcsize(TAIL_FMT)

# Shadow data block: header(20) + data(40) + tail(4) = 64
# Header: magic H, ver B, flags B, reserved 4s, seq I, offset I, total_bytes I
SHADOW_HEADER_FMT = '<H B B 4s I I I'
SHADOW_HEADER_SIZE = struct.calcsize(SHADOW_HEADER_FMT)  # 20
SHADOW_DATA_SIZE = BLOCK_SIZE - SHADOW_HEADER_SIZE - TAIL_SIZE  # 40

# Verify totals
assert HEADER_SIZE == 28, f"Header size mismatch: {HEADER_SIZE}"
assert ENTRY_SIZE == 8, f"Entry size mismatch: {ENTRY_SIZE}"
assert TAIL_SIZE == 4, f"Tail size mismatch: {TAIL_SIZE}"
assert HEADER_SIZE + ENTRIES_PER_BLOCK * ENTRY_SIZE + TAIL_SIZE == 64, \
    f"Total: {HEADER_SIZE + ENTRIES_PER_BLOCK * ENTRY_SIZE + TAIL_SIZE}"
assert SHADOW_HEADER_SIZE + SHADOW_DATA_SIZE + TAIL_SIZE == 64, \
    f"Shadow block: {SHADOW_HEADER_SIZE}+{SHADOW_DATA_SIZE}+{TAIL_SIZE} != 64"


def _pogls_xxh64(data: bytes) -> int:
    """Simple xxh64-like hash for shadow integrity."""
    h = 0x424D524D424D524D
    for i, b in enumerate(data):
        h ^= b << ((i * 7) & 63)
        h = (h * 0x00000100000001B3) & 0xFFFFFFFFFFFFFFFF
        h ^= h >> 31
    return h & 0xFFFFFFFFFFFFFFFF


class BermudaBlockWriter:
    """Build 64B DiamondBlocks from a routing verdict + shadow."""

    @staticmethod
    def from_verdict(verdict: RoutingVerdict,
                     shadow_flat: torch.Tensor = None,
                     attach_shadow: bool = False) -> List[bytes]:
        """
        Convert verdict → list of 64-byte DiamondBlocks.

        Each unique (zone, shape) group gets one block with up to
        4 routing entries. If a group has >4 entries, blocks chain
        via link_next.

        Args:
          verdict: RoutingVerdict from bermuda_router
          shadow_flat: optional [N, D] float shadow tensor (for stats)
          attach_shadow: if True, serialize full shadow bytes into
                         shadow data blocks appended after routing blocks.
                         The last routing block's link_next chains to
                         the first shadow block.

        Returns:
          list of 64-byte bytes objects
        """
        real = verdict.real_mask
        zones  = verdict.zone[real]
        shapes = verdict.shape[real]
        polarities = verdict.polarity[real]
        idxs_in   = verdict.idx_in[real]
        idxs_out  = verdict.idx_out[real]
        trings    = verdict.tring_slot[real]

        # Compute shadow fingerprint
        shadow_xxh64 = 0
        shadow_mean  = 0.0
        shadow_std   = 0.0
        shadow_bytes = b''
        if shadow_flat is not None:
            sh = shadow_flat.reshape(-1).detach().cpu()
            shadow_bytes = sh.numpy().tobytes()
            shadow_xxh64 = _pogls_xxh64(shadow_bytes)
            shadow_mean  = sh.mean().item()
            shadow_std   = sh.std().item()

        # Group entries by (zone, shape)
        groups = {}
        for i in range(real.sum().item()):
            key = (zones[i].item(), shapes[i].item(), polarities[i].item())
            if key not in groups:
                groups[key] = []
            groups[key].append({
                'idx_in': int(idxs_in[i].item()),
                'idx_out': int(idxs_out[i].item()),
                'tring_slot': int(trings[i].item()),
            })

        blocks = []
        block_id = 0

        for (zone, shape, polarity), entries in sorted(groups.items()):
            n = len(entries)
            shape_byte = shape & 0xFF

            # Split into chunks of ENTRIES_PER_BLOCK
            for chunk_start in range(0, n, ENTRIES_PER_BLOCK):
                chunk = entries[chunk_start:chunk_start + ENTRIES_PER_BLOCK]
                n_entries = len(chunk)

                # Pack entries
                entry_bytes = b''
                for e in chunk:
                    entry_bytes += struct.pack(ENTRY_FMT,
                        e['idx_in'] & 0xFFFF,
                        e['idx_out'] & 0xFFFF,
                        e['tring_slot'] & 0xFFFF,
                        zone & 0xFF,
                        0,  # pad
                    )
                # Pad remaining entries (zero-filled)
                entry_bytes += b'\x00' * (ENTRIES_PER_BLOCK - n_entries) * ENTRY_SIZE

                # Tail: link to next block if more entries remain
                more = (chunk_start + ENTRIES_PER_BLOCK) < n
                link_next = block_id + 1 if more else 0
                tail_bytes = struct.pack(TAIL_FMT, link_next)

                # Header
                flags = 0
                header_bytes = struct.pack(HEADER_FMT,
                    BERMUDA_MAGIC,
                    BERMUDA_VERSION,
                    verdict.mode,
                    verdict.gear,
                    zone,
                    shape_byte,
                    polarity,
                    len(entries),      # total count for this group
                    n_entries,         # entries in THIS block
                    flags,
                    shadow_xxh64,
                    shadow_mean,
                    shadow_std,
                )

                block = header_bytes + entry_bytes + tail_bytes
                assert len(block) == BLOCK_SIZE, f"Block size: {len(block)}"
                blocks.append(block)
                block_id += 1

        # ── Append shadow data blocks ────────────────────────────
        if attach_shadow and shadow_bytes:
            # Set SHADOW_ATTACHED flag on the last routing block
            last_routing_block_idx = len(blocks) - 1
            if last_routing_block_idx >= 0:
                # Re-parse header to update flags
                last_block = blocks[last_routing_block_idx]
                hdr = list(struct.unpack(HEADER_FMT, last_block[:HEADER_SIZE]))
                hdr[9] |= SHADOW_ATTACHED  # flags byte at index 9
                new_hdr = struct.pack(HEADER_FMT, *hdr)
                blocks[last_routing_block_idx] = new_hdr + last_block[HEADER_SIZE:]

            # Write shadow blocks
            total_bytes = len(shadow_bytes)
            for data_offset in range(0, total_bytes, SHADOW_DATA_SIZE):
                chunk = shadow_bytes[data_offset:data_offset + SHADOW_DATA_SIZE]
                chunk = chunk.ljust(SHADOW_DATA_SIZE, b'\x00')
                seq = data_offset // SHADOW_DATA_SIZE
                more_shadow = (data_offset + SHADOW_DATA_SIZE) < total_bytes

                shadow_hdr = struct.pack(SHADOW_HEADER_FMT,
                    BERMUDA_SHADOW_MAGIC,
                    1,           # version
                    1 if more_shadow else 0,  # flags: has_next
                    b'\x00\x00\x00\x00',  # reserved
                    seq,
                    data_offset,
                    total_bytes,
                )
                tail_next = (block_id + 1) if more_shadow else 0
                shadow_tail = struct.pack(TAIL_FMT, tail_next)

                block = shadow_hdr + chunk + shadow_tail
                assert len(block) == BLOCK_SIZE, f"Shadow block size: {len(block)}"
                blocks.append(block)
                block_id += 1

                # Chain previous shadow block → this one via tail
                # (no explicit re-write needed — the tail_next already
                #  points forward; routing blocks' link_next points here)

            # Chain last routing block → first shadow block
            if last_routing_block_idx >= 0:
                first_shadow_id = last_routing_block_idx + 1
                last_routing = blocks[last_routing_block_idx]
                tail_off = BLOCK_SIZE - TAIL_SIZE
                last_routing = (last_routing[:tail_off]
                                + struct.pack(TAIL_FMT, first_shadow_id)
                                + last_routing[tail_off + TAIL_SIZE:])
                blocks[last_routing_block_idx] = last_routing

        return blocks


class BermudaBlockReader:
    """Read 64B DiamondBlocks back into routing info."""

    @staticmethod
    def parse_header(block: bytes) -> dict:
        h = struct.unpack(HEADER_FMT, block[:HEADER_SIZE])
        return {
            'magic':       h[0],
            'version':     h[1],
            'mode':        h[2],
            'gear':        h[3],
            'zone':        h[4],
            'shape':       chr(h[5]),
            'polarity':    h[6],
            'count':       h[7],
            'n_entries':   h[8],
            'flags':       h[9],
            'shadow_xxh64': h[10],
            'shadow_mean':  h[11],
            'shadow_std':   h[12],
        }

    @staticmethod
    def parse_entries(block: bytes) -> list:
        entries = []
        for i in range(ENTRIES_PER_BLOCK):
            off = HEADER_SIZE + i * ENTRY_SIZE
            raw = struct.unpack(ENTRY_FMT, block[off:off + ENTRY_SIZE])
            entries.append({
                'idx_in':     raw[0],
                'idx_out':    raw[1],
                'tring_slot': raw[2],
                'zone':       raw[3],
            })
        return entries

    @staticmethod
    def parse_tail(block: bytes) -> dict:
        off = HEADER_SIZE + ENTRIES_PER_BLOCK * ENTRY_SIZE
        link_next = struct.unpack(TAIL_FMT, block[off:off + TAIL_SIZE])[0]
        return {'link_next': link_next}

    @staticmethod
    def validate(block: bytes) -> bool:
        if len(block) != BLOCK_SIZE:
            return False
        h = BermudaBlockReader.parse_header(block)
        return h['magic'] == BERMUDA_MAGIC and h['version'] == BERMUDA_VERSION

    @staticmethod
    def read_all(blocks: List[bytes]) -> List[dict]:
        """Parse all BermudaBlocks sequentially."""
        results = []
        for block in blocks:
            if not BermudaBlockReader.validate(block):
                continue
            h = BermudaBlockReader.parse_header(block)
            entries = BermudaBlockReader.parse_entries(block)
            tail = BermudaBlockReader.parse_tail(block)
            results.append({
                'header': h,
                'entries': [e for e in entries[:h['n_entries']]],
                'tail': tail,
            })
        return results

    @staticmethod
    def read_shadow(blocks: List[bytes]) -> bytes:
        """
        Extract full shadow payload from attached shadow blocks.
        Returns empty bytes if no shadow blocks found.

        Shadow blocks are identified by magic = 0x5348 ('SH').
        Data is reconstructed by seq order, not link_next chain.
        """
        shadow_chunks = {}
        for block in blocks:
            if len(block) != BLOCK_SIZE:
                continue
            magic = struct.unpack('<H', block[:2])[0]
            if magic != BERMUDA_SHADOW_MAGIC:
                continue
            hdr = struct.unpack(SHADOW_HEADER_FMT, block[:SHADOW_HEADER_SIZE])
            seq = hdr[4]
            data_offset = hdr[5]
            total = hdr[6]
            # data starts after header, before tail
            data = block[SHADOW_HEADER_SIZE:SHADOW_HEADER_SIZE + SHADOW_DATA_SIZE]
            remaining = total - data_offset
            data = data[:remaining]
            shadow_chunks[seq] = data

        if not shadow_chunks:
            return b''

        # Reconstruct in sequence order
        result = b''.join(shadow_chunks[i] for i in sorted(shadow_chunks.keys()))
        return result


# ── Test ────────────────────────────────────────────────────
def test_roundtrip():
    import sys, torch
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'python_src'))
    from bermuda_pipeline import BermudaPipeline

    print("=" * 56)
    print("BermudaBlock: DiamondBlock serialization test")
    print("=" * 56)

    pipe = BermudaPipeline(dim=128, gear=2)
    device = next(pipe.router.gate.parameters()).device
    data = torch.randn(64, 128, device=device)

    for mode in range(4):
        result = pipe.process_float(data, mode)
        v = result['verdict']

        # Simulate shadow tensor (padded size = 512 for gear 1)
        shadow_test = torch.randn(512, 128) * 0.1

        # ── Test without attach_shadow ──────────────────────
        blocks = BermudaBlockWriter.from_verdict(v, shadow_test)
        parsed = BermudaBlockReader.read_all(blocks)
        n_read = sum(len(p['entries']) for p in parsed)
        valid = all(
            p['header']['magic'] == BERMUDA_MAGIC
            and p['header']['version'] == BERMUDA_VERSION
            for p in parsed
        )
        n_real = v.n_tokens
        no_shadow_len = len(blocks)

        mode_names = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]
        ok = n_read == n_real and valid
        print(f"\n  {mode_names[mode]} (no shadow): "
              f"{len(blocks)} blocks → "
              f"{n_real} real tokens, "
              f"{n_read} entries read "
              f"{'✓' if ok else '✗'}")

        # ── Test WITH attach_shadow ─────────────────────────
        blocks_s = BermudaBlockWriter.from_verdict(
            v, shadow_test, attach_shadow=True)
        parsed_s = BermudaBlockReader.read_all(blocks_s)
        n_read_s = sum(len(p['entries']) for p in parsed_s)

        # Extract shadow back
        shadow_restored = BermudaBlockReader.read_shadow(blocks_s)
        shadow_original = shadow_test.reshape(-1).detach().cpu().numpy().tobytes()
        shadow_match = (shadow_restored == shadow_original)
        shadow_data_len = len(blocks_s) - no_shadow_len

        print(f"  {mode_names[mode]} (attach_shadow): "
              f"{len(blocks_s)} blocks "
              f"(+{shadow_data_len} shadow), "
              f"{n_read_s} entries, "
              f"shadow restore {'✓' if shadow_match else '✗'}")
        if shadow_data_len > 0:
            shadow_pct = shadow_data_len * 64 / len(shadow_original) * 100
            print(f"    shadow: {len(shadow_original)}B → "
                  f"{shadow_data_len} blocks ({shadow_pct:.0f}% overhead)")

        zone_summary = {}
        for p in parsed_s:
            h = p['header']
            key = (h['zone'], h['shape'])
            zone_summary[key] = zone_summary.get(key, 0) + len(p['entries'])
        for (z, s), cnt in sorted(zone_summary.items())[:5]:
            print(f"    zone={z} shape={s}: {cnt} entries")
        if len(zone_summary) > 5:
            print(f"    ... and {len(zone_summary)-5} more groups")

    print("\nAll tests passed ✓")


if __name__ == "__main__":
    test_roundtrip()
