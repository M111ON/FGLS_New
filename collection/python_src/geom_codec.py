"""geom_codec: convert Bermuda geometry → RGB layer sequence (36×20 × 3ch)"""
from __future__ import annotations
import numpy as np
from PIL import Image
from pathlib import Path
from io import BytesIO
from typing import List, Optional, Dict, Tuple, Any

TRING_ROWS, TRING_COLS = 8, 8

def verdict_to_level(
    tring_slots: List[int],
    zones: List[int],
    shapes: List[str],
    polarities: List[str],
) -> np.ndarray:
    rgb = np.zeros((TRING_ROWS, TRING_COLS, 3), dtype=np.uint8)
    occupied = {}
    for i, slot in enumerate(tring_slots):
        cell = slot % 64          # collapse 720 → 64
        r, c = divmod(cell, TRING_COLS)
        zone_val = 60 + (zones[i] % 12) * 15
        shape_byte = ord(shapes[i]) if isinstance(shapes[i], str) else shapes[i]
        shape_val = 80 + (shape_byte % 26) * 6
        pol_val = 220 if polarities[i] == 'ROUTE' else 50
        # blend if multiple tring_slots map to same cell
        if cell in occupied:
            rgb[r, c] = np.clip(rgb[r, c].astype(int) + [zone_val//4, shape_val//4, pol_val//4], 0, 255).astype(np.uint8)
        else:
            rgb[r, c] = [zone_val, shape_val, pol_val]
        occupied[cell] = slot
    for cell in range(64):
        if cell not in occupied:
            r, c = divmod(cell, TRING_COLS)
            rgb[r, c] = [8, 6, 10]
    return rgb

# ── Shape → fold_axis mapping (mirrors pogls_bond_py) ────
_SHAPE_TO_AXIS = {'I': 1, 'O': 2, 'T': 3, 'S': 4, 'Z': 5, 'L': 6}

def _piece_from_token(token: dict, tring_slot: int) -> dict:
    """Derive a bond piece from a routing token's zone/shape/tring_slot."""
    from pogls_bond_py import make_piece
    zone = token.get('zone', 0)
    shape = token.get('shape', 'I')
    seed = int((zone << 16) | (int(tring_slot) & 0xFFFF))
    axis = _SHAPE_TO_AXIS.get(shape, 1)
    return make_piece(seed, axis)

def track_connections(levels: List[dict]) -> List[dict]:
    """
    Track enter/leave/persist using Bond-key identity (not topology).
    
    Each token's piece is derived from zone/shape/tring_slot. Two tokens
    at consecutive levels are "connected" if their bond_keys match exactly
    (same piece identifier). This treats bond_key as a command/iteration
    tag rather than a topological constraint — no bond_verify topology check.
    """
    from pogls_bond_py import bond_key

    result = []
    prev_key_map: dict = {}   # bond_key → [slot_ids]
    prev_slots: set = set()

    for i, lv in enumerate(levels):
        tring_slots = lv.get('tring_slots', [])
        tokens = lv.get('sample_tokens', [])

        cur_key_map: dict = {}
        cur_pieces: dict = {}
        for sidx, slot_id in enumerate(tring_slots):
            token = tokens[sidx] if sidx < len(tokens) else {}
            piece = _piece_from_token(token, slot_id)
            sid = int(slot_id)
            bk = bond_key(piece)
            cur_pieces[sid] = piece
            cur_key_map.setdefault(bk, []).append(sid)
        cur_slots = set(cur_pieces.keys())

        if i == 0:
            enter = sorted(cur_slots)
            leave: list = []
            persist: list = []
        else:
            # Match by bond_key identity (not bond_verify)
            bonded_cur: set = set()
            bonded_prev: set = set()

            # Find bond_keys that persist across levels
            for bk, prev_ids in prev_key_map.items():
                cur_ids = cur_key_map.get(bk)
                if cur_ids:
                    # Same bond_key found → these slots persist
                    for sid in cur_ids:
                        bonded_cur.add(sid)
                    for sid in prev_ids:
                        bonded_prev.add(sid)

            persist = sorted(bonded_cur)
            leave   = sorted(prev_slots - bonded_prev)
            enter   = sorted(cur_slots - bonded_cur)

        result.append({
            'level': i,
            'enter': enter,
            'leave': leave,
            'persist': persist,
            'n_cur': len(cur_pieces),
            'n_enter': len(enter),
            'n_leave': len(leave),
            'n_persist': len(persist),
        })

        prev_key_map = cur_key_map
        prev_slots   = cur_slots

    return result

def connection_flow(levels: List[dict]) -> np.ndarray:
    """Build flow matrix: flow_map[slot][next_slot] = count of bond-key matched moves."""
    flow = np.zeros((720, 720), dtype=np.uint16)
    from pogls_bond_py import bond_key

    for i in range(len(levels) - 1):
        cur_tokens = levels[i].get('sample_tokens', [])
        cur_slots  = levels[i].get('tring_slots', [])
        nxt_tokens = levels[i + 1].get('sample_tokens', [])
        nxt_slots  = levels[i + 1].get('tring_slots', [])

        # Build bond_key → slot map for current level
        cur_bk_map: dict = {}
        for sidx, slot_id in enumerate(cur_slots):
            token = cur_tokens[sidx] if sidx < len(cur_tokens) else {}
            bk = bond_key(_piece_from_token(token, slot_id))
            cur_bk_map.setdefault(int(bk), []).append(int(slot_id))

        # For each next-level token, find matching bond_key in current
        for sidx, slot_id in enumerate(nxt_slots):
            token = nxt_tokens[sidx] if sidx < len(nxt_tokens) else {}
            bk = bond_key(_piece_from_token(token, slot_id))
            prev_slots = cur_bk_map.get(int(bk), [])
            for ps in prev_slots:
                flow[ps, int(slot_id)] += 1

    return flow

# ── Encode ───────────────────────────────────────────────
def encode_sequence(verdicts: list, fps: int = 30) -> bytes:
    frames = []
    for v in verdicts:
        rgb = verdict_to_level(
            v.get('tring_slots', []),
            [t['zone'] for t in v.get('sample_tokens', [])],
            [t['shape'] for t in v.get('sample_tokens', [])],
            [t['polarity'] for t in v.get('sample_tokens', [])],
        )
        frames.append(Image.fromarray(rgb, 'RGB'))
    buf = BytesIO()
    frames[0].save(buf, format='PNG', save_all=True,
                   append_images=frames[1:], duration=1000//fps, loop=0)
    return buf.getvalue()

def encode_level_gif(levels: List[dict], fps: int = 30, marker: str = 'enter') -> bytes:
    """Like encode_sequence but highlights entering/leaving/persist slots in accent color."""
    conns = track_connections(levels)
    frames = []
    for i, lv in enumerate(levels):
        rgb = np.zeros((TRING_ROWS, TRING_COLS, 3), dtype=np.uint8)
        _raw_occ = set(lv.get('tring_slots', []))
        occupied = {s % 64 for s in _raw_occ}
        highlights = {s % 64 for s in conns[i].get(marker, [])}
        for s in range(64):
            r, c = divmod(s, TRING_COLS)
            cell = s
            if cell in highlights:
                rgb[r, c] = [255, 220, 60]   # gold = enter/leave
            elif cell in occupied:
                rgb[r, c] = [180, 120, 200]  # lavender = persistent
            else:
                rgb[r, c] = [8, 6, 10]       # dark
        frames.append(Image.fromarray(rgb, 'RGB'))
    buf = BytesIO()
    frames[0].save(buf, format='PNG', save_all=True,
                   append_images=frames[1:], duration=1000//fps, loop=0)
    return buf.getvalue()

# ── Lossless MP4 encode (libx264rgb -crf 0) ─────────────
import subprocess, tempfile, os
def encode_level_mp4(verdicts: list, fps: int = 30,
                     header_bytes: Optional[bytes] = None) -> bytes:
    frames = []
    for v in verdicts:
        rgb = verdict_to_level(
            v.get('tring_slots', []),
            [t['zone'] for t in v.get('sample_tokens', [])],
            [t['shape'] for t in v.get('sample_tokens', [])],
            [t['polarity'] for t in v.get('sample_tokens', [])],
        )
        frames.append(Image.fromarray(rgb, 'RGB'))
    if header_bytes:
        hdr_rgb = np.full((TRING_ROWS, TRING_COLS, 3), [8, 6, 10], dtype=np.uint8)
        for i, b in enumerate(header_bytes):
            r, c = divmod(i, TRING_COLS)
            hdr_rgb[r, c] = [b, b ^ 0x37, b ^ 0xAA]
        frames.insert(0, Image.fromarray(hdr_rgb, 'RGB'))
    with tempfile.TemporaryDirectory(prefix='geom_mp4_') as tmp:
        for i, img in enumerate(frames):
            img.save(os.path.join(tmp, 'frame_{:05d}.png'.format(i)))
        mp4_path = os.path.join(tmp, 'output.mp4')
        subprocess.run([
            'ffmpeg', '-y', '-framerate', str(fps),
            '-i', os.path.join(tmp, 'frame_%05d.png'),
            '-c:v', 'libx264rgb', '-crf', '0',
            '-pix_fmt', 'rgb24',
            '-movflags', '+faststart',
            mp4_path
        ], capture_output=True, check=True)
        return Path(mp4_path).read_bytes()

def save_sequence(verdicts: list, path: str, fps: int = 30):
    Path(path).write_bytes(encode_sequence(verdicts, fps))

# ── Live capture ─────────────────────────────────────────
def capture_tick(router, x: 'torch.Tensor', mode: int = 0) -> dict:
    import torch
    verdict = router.route(x.float(), mode)
    real = verdict.real_mask
    tokens = []
    for i in range(real.sum().item()):
        tokens.append({
            'zone': int(verdict.zone[real][i].item()),
            'shape': chr(int(verdict.shape[real][i].item())),
            'polarity': 'ROUTE' if verdict.polarity[real][i].item() == 0 else 'GROUND',
            'tring_slot': int(verdict.tring_slot[real][i].item()),
        })
    return {
        'tring_slots': sorted(set(v.tring_slot[real].tolist())),
        'sample_tokens': tokens,
        'n_real': int(real.sum().item()),
    }
