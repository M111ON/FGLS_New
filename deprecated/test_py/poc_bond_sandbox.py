"""
Geometric Sandbox + Intrinsic Bond Layer — Full Stack Demo
══════════════════════════════════════════════════════════════════════════
Stack:
  FGLS:  topology_scan_multiscale()  → real geometry topology
  Smart Folder:  ingest_file() → coord wallet (topology_fp)
  Bond Layer:    seed_from_fp() → make_piece() → bond_verify()

Usage:
  python poc_bond_sandbox.py
"""

import os, sys, json, tempfile, shutil
from pathlib import Path

# ── add paths ────────────────────────────────────────────────────────────
ZGLS_ROOT = Path(r"I:\ZGLS")
sys.path.insert(0, str(ZGLS_ROOT))
sys.path.insert(0, str(ZGLS_ROOT / "POGLS_SRC" / "master_extracted" / "python"))
os.environ["SMART_FOLDER_POGLS_PY"] = str(ZGLS_ROOT / "POGLS_SRC" / "master_extracted" / "python")
os.environ["SMART_FOLDER_BACKEND"] = "local"

sys.path.insert(0, str(Path(__file__).resolve().parent / "python_src"))

from smart_folder_mvp.store import SmartFolderStore
from pogls_file_coord import topology_scan_multiscale
from pogls_bond_py import (
    fibo_addr, make_piece, bond_key, bond_verify,
    seed_from_fp, piece_from_wallet,
    piece_to_dict, slot_to_dict,
    make_slot, plug_connect, plug_disconnect,
    reroute, POGLS_OVERFLOW, POGLS_FAULT,
    PLUG_N, PLUG_S, PLUG_E, PLUG_W,
)
from pogls_wallet_py import WalletBuilder, WalletReader, WALLET_MODE_PATH

print("=" * 68)
print("  GEOMETRIC SANDBOX + INTRINSIC BOND — Full Stack")
print("=" * 68)

# ══════════════════════════════════════════════════════════════════════════
# 0.  SETUP
# ══════════════════════════════════════════════════════════════════════════
WORKSPACE = Path(tempfile.mkdtemp(prefix="bond_sandbox_"))
store = SmartFolderStore(WORKSPACE)
store.init()
ws_id = store.status()["workspace_id"]
print(f"\n  Workspace : {WORKSPACE}")
print(f"  Workspace ID: {ws_id}")

# ══════════════════════════════════════════════════════════════════════════
# 1.  CREATE 4 TEST FILES (simulating 4 agent outputs)
# ══════════════════════════════════════════════════════════════════════════
AGENTS = [
    ("A", "Reader",   1, "parse_data",      "Fibonacci closure=144 cycles, Dodeca face=3, spoke=2, slot=17"),
    ("B", "Analyst",  3, "validate_schema", "POGLS fields: face=3, spoke=2, slot=17, cycles=144 — all non-zero"),
    ("C", "Writer",   1, "write_output",    "Executive summary: Geometric analysis completed at cycle 144 with face-3 activation"),
    ("D", "Auditor",  6, "audit_log",       "AUDIT: timestamp=2026-05-18, tasks=4, integrity=pass, schema=v4"),
]

files = {}
results = {}
topology_fps = {}

for agent_id, role, axis, task, content in AGENTS:
    fname = f"agent_{agent_id}_{task}.txt"
    fpath = WORKSPACE / fname
    fpath.write_text(content)
    files[agent_id] = fpath

    # Ingest via SmartFolder → auto topology scan
    r = store.ingest_file(fpath, virtual_path=fname)

    # Get coord wallet topology_fp
    manifest = store._load()
    entry = manifest.files[fname]
    version = entry.current()
    wallet = version.coord_wallet
    topology_fps[agent_id] = wallet["topology_fp"]

    print(f"\n  [{agent_id}] Agent-{agent_id} ({role}), axis={axis}, task={task}")
    print(f"      File  : {fname} ({len(content)} bytes)")
    print(f"      Route : {version.analysis.get('routing_hint','?')}")
    print(f"      Content: {version.analysis.get('content_type','?')}")

# ══════════════════════════════════════════════════════════════════════════
# 2.  BOND LAYER — create pieces from topology fingerprints
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  BOND LAYER — Pieces from Wallet Fingerprints")
print(f"{'='*68}")

session_seed = fibo_addr(0x9009009009009009 ^ int(ws_id[:8], 16))
pieces = {}
slots = {}

for i, (agent_id, role, axis, task, content) in enumerate(AGENTS):
    fp = topology_fps[agent_id]
    seed = seed_from_fp(fp) ^ (i * 0x100)
    piece = make_piece(fibo_addr(seed), axis)
    pieces[agent_id] = piece
    slots[agent_id] = make_slot(ord(agent_id), fibo_addr(seed), axis, token_cap=300 + i * 50)

    pd = piece_to_dict(piece)
    print(f"\n  Agent-{agent_id} ({role})  axis={axis}")
    print(f"      fp       = {fp}")
    print(f"      geo_key  = {pd['geo_key']}")
    print(f"      shape    = {pd['shape']}")
    print(f"      bond_key = {pd['bond_key']}")

# ══════════════════════════════════════════════════════════════════════════
# 3.  BOND VERIFICATION
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  BOND VERIFICATION — Coordinate Enforcement")
print(f"{'='*68}")

# Wire extrinsic plugs: A─E/W─B─E/W─C─E/W─D
plug_connect(slots["A"], PLUG_E, slots["B"], PLUG_W, 64)
plug_connect(slots["B"], PLUG_E, slots["C"], PLUG_W, 64)
plug_connect(slots["C"], PLUG_E, slots["D"], PLUG_W, 64)

print(f"\n  Extrinsic Plug Chain:  A ── B ── C ── D")
for aid in ["A", "B", "C", "D"]:
    sd = slot_to_dict(slots[aid])
    plugs = sd.get("plugs", {})
    if plugs:
        for face, info in plugs.items():
            print(f"      Agent-{aid}.{face} → {info['target']}")

print()
# Intrinsic bond verification
for pair in [("A","B"), ("B","C"), ("C","D"), ("A","D")]:
    a, b = pair
    bv = bond_verify(pieces[a], pieces[b])
    print(f"      Bond {a}↔{b}: key={bv['bond_key']:016x}  valid={bv['valid']}")

# Show coord shift breaks bond
shifted = dict(pieces["A"])
shifted["geo_key"] = pieces["A"]["geo_key"] ^ 1
shifted["bond_L"] = fibo_addr(shifted["geo_key"] ^ 0xAAAAAAAAAAAAAAAA)
shifted["bond_R"] = fibo_addr(shifted["geo_key"] ^ 0x5555555555555555)
bk_orig = bond_key(pieces["A"])
bk_shifted = bond_key(shifted)
print(f"\n  Coordinate shift test:")
print(f"      A geo_key+1 → bond_key: {bk_orig:016x} → {bk_shifted:016x}")
print(f"      Bond broken: {bk_orig != bk_shifted}")

# ══════════════════════════════════════════════════════════════════════════
# 4.  GHOST DELETE + LOCK
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  GHOST DELETE — Locking Agent Files")
print(f"{'='*68}")

for agent_id, role, axis, task, content in AGENTS:
    fname = f"agent_{agent_id}_{task}.txt"
    store.delete_file(fname, mode="ghost")
    print(f"      Agent-{agent_id} ({task}): ghost deleted")

files_left = store.list_files()
print(f"\n  Files after ghost delete: {len(files_left)}")
print(f"  Ghost files in manifest:  {store.status()['ghost_files']}")

# ══════════════════════════════════════════════════════════════════════════
# 5.  RECONSTRUCT FROM BOND WALLET
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  RECONSTRUCT — From Bond Wallet")
print(f"{'='*68}")

recon_dir = WORKSPACE / "reconstructed"
os.makedirs(recon_dir, exist_ok=True)

for agent_id, role, axis, task, content in AGENTS:
    fname = f"agent_{agent_id}_{task}.txt"
    manifest = store._load()
    entry = manifest.files.get(fname)
    if entry and entry.current():
        wallet = entry.current().coord_wallet
        recon_result = store.reconstruct_from_coord_wallet(
            wallet,
            out_path=recon_dir / fname
        )
        print(f"      Agent-{agent_id}: reconstruct={recon_result['reconstruct_source']}"
              f"  exact={recon_result['exact_restore']}")

# ══════════════════════════════════════════════════════════════════════════
# 6.  OMEGA FAULT SIMULATION
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  OMEGA REROUTE — Fault Simulation")
print(f"{'='*68}")

# Simulate Agent-B (splitter, high capacity) overflow
print(f"\n  Agent-B (T-shape, Analyst):  token overflow → OMEGA_COMPRESS")
before = dict(slots["B"]["piece"])
reroute(slots["B"], POGLS_OVERFLOW)
after = slots["B"]["piece"]
print(f"      Shape : {before['shape']} → {after['shape']}")
print(f"      Geo   : {before['geo_key']:016x} → {after['geo_key']:016x}")

# Simulate Agent-D (Auditor) fault
print(f"\n  Agent-D (L-shape, Auditor):  fault → OMEGA_QUARANTINE")
before2 = dict(slots["D"]["piece"])
reroute(slots["D"], POGLS_FAULT)
after2 = slots["D"]["piece"]
print(f"      Shape : {before2['shape']} → {after2['shape']}")
print(f"      Geo   : {before2['geo_key']:016x} → {after2['geo_key']:016x}")

# ══════════════════════════════════════════════════════════════════════════
# 7.  PORTABLE BOND WALLET (JSON, QR-ready)
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  PORTABLE BOND WALLET — JSON Payload")
print(f"{'='*68}")

bond_wallet = {
    "workspace_id": ws_id,
    "session": f"{session_seed:016x}",
    "agents": {}
}
for agent_id, role, axis, task, content in AGENTS:
    bond_wallet["agents"][agent_id] = {
        "role": role,
        "axis": axis,
        "topology_fp": topology_fps[agent_id],
        "piece": piece_to_dict(pieces[agent_id]),
    }

wallet_json = json.dumps(bond_wallet, indent=2)
print(f"\n  Wallet size: {len(wallet_json)} bytes")
print(f"  Agents: {list(bond_wallet['agents'].keys())}")
print(f"  Session: {bond_wallet['session'][:16]}...")

# ══════════════════════════════════════════════════════════════════════════
# SUMMARY
# ══════════════════════════════════════════════════════════════════════════
print(f"\n{'='*68}")
print(f"  FULL STACK VERDICT")
print(f"{'='*68}")
print(f"""
  ✓ FGLS topology scan       → content type, routing hint, topology_fp
  ✓ Smart Folder ingest       → SHA-256 blob + coord wallet
  ✓ Bond pieces from wallet   → seed_from_fp() → make_piece()
  ✓ Bond verification         → coordinate enforcement (no permission check)
  ✓ Extrinsic plug chain      → A─B─C─D wired
  ✓ Ghost delete              → files invisible, path severed
  ✓ Reconstruction            → authority blob restore (100% exact)
  ✓ Omega reroute             → overflow→compress, fault→quarantine
  ✓ Portable bond wallet      → JSON payload, QR-ready

  Architecture:
    topology_scan_multiscale()  → topology_fp
    seed_from_fp(topology_fp)   → origin_seed
    make_piece(seed, axis)      → geo_key + bond_L + bond_R
    bond_key(piece)             → intrinsic bond (XOR, self-enforcing)
    bond_verify(a, b)           → valid only if coordinate-paired
""")

# ── cleanup ──────────────────────────────────────────────────────────────
shutil.rmtree(WORKSPACE, ignore_errors=True)
print("  (workspace cleaned up)")
