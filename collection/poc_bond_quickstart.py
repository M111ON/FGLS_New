"""
Geometric Bond + Sandbox — Quick Start
═════════════════════════════════════════════════════════════════
รัน:  python collection\poc_bond_quickstart.py

จะเห็นทั้งหมด:  C bond → Python bond → ghost delete → foldgate mock
"""

import subprocess, sys, os
from pathlib import Path

COLLECTION = Path(__file__).resolve().parent
PYTHON_SRC = COLLECTION / "python_src"

def section(title):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")

# ══════════════════════════════════════════════════════════════════════════
# 1.  C BOND TEST
# ══════════════════════════════════════════════════════════════════════════
section("1.  C BOND LAYER (test_pogls_bond.c)")
print("  คำสั่ง:  gcc -O2 -I. -o build/pogls_bond_test test_pogls_bond.c && build/pogls_bond_test")

# rebuild if needed
bond_exe = COLLECTION / "build" / "pogls_bond_test"
if not bond_exe.exists():
    subprocess.run(["gcc", "-O2", "-I.", "-o", str(bond_exe),
                    str(COLLECTION / "test_pogls_bond.c")],
                   cwd=COLLECTION, capture_output=True)

r = subprocess.run([str(bond_exe)], cwd=COLLECTION, capture_output=True, text=True)
print(r.stdout)
print(f"  → C tests: {'PASS' if 'ALL TESTS PASSED' in r.stdout else 'FAIL'}")

# ══════════════════════════════════════════════════════════════════════════
# 2.  PYTHON BOND TESTS
# ══════════════════════════════════════════════════════════════════════════
section("2.  PYTHON BOND LAYER (pogls_bond_py.py)")
print("  คำสั่ง:  python python_src/pogls_bond_py.py")

r = subprocess.run([sys.executable, str(PYTHON_SRC / "pogls_bond_py.py")],
                   capture_output=True, text=True)
print(r.stdout)

# ══════════════════════════════════════════════════════════════════════════
# 3.  CROSS-LANGUAGE VERIFICATION
# ══════════════════════════════════════════════════════════════════════════
section("3.  C ↔ PYTHON CROSS-LANGUAGE")
print("  คำสั่ง:  python python_src/test_cross_consistency.py")

r = subprocess.run([sys.executable, str(PYTHON_SRC / "test_cross_consistency.py")],
                   capture_output=True, text=True)
for line in r.stdout.splitlines():
    if "PASS" in line or "FAIL" in line or "MATCH" in line or "True" in line or "False" in line:
        if "bond_key" not in line and "valid" not in line and "XOR" not in line:
            print(f"  {line.strip()}")

# ══════════════════════════════════════════════════════════════════════════
# 4.  FULL-STACK BOND + SANDBOX DEMO
# ══════════════════════════════════════════════════════════════════════════
section("4.  FULL-STACK DEMO (poc_bond_sandbox.py)")
print("  คำสั่ง:  python poc_bond_sandbox.py")
print("  (กำลังรอ 2-3 วินาที...)")
print("  ── ตัวอย่าง output ──")

r = subprocess.run([sys.executable, str(COLLECTION / "poc_bond_sandbox.py")],
                   capture_output=True, text=True)
lines = r.stdout.splitlines()
summary_lines = [l for l in lines if "✓" in l or "✗" in l or "Architecture" in l or "Stack" in l or "Sandbox" in l]
for l in lines[-25:]:
    if l.strip():
        print(f"  {l.strip()}")

# ══════════════════════════════════════════════════════════════════════════
# 5.  QUICK API REFERENCE
# ══════════════════════════════════════════════════════════════════════════
section("5.  API Reference")
print("""
  Python API (pogls_bond_py.py):
    fibo_addr(seed)             → uint64  (C-compatible deterministic hash)
    make_piece(origin_seed, axis) → piece dict  (geo_key, shape, bond_L, bond_R)
    bond_key(piece)             → uint64  (bond_L ^ bond_R)
    bond_verify(a, b)           → {"bond_key": ..., "valid": bool}
    seed_from_fp(topology_fp)   → uint64  (wallet → C bridge)
    piece_from_wallet(fp, axis) → piece dict
    make_slot(agent_id, seed, axis) → slot dict  (+ token_cap, plugs)
    plug_connect(a, faceA, b, faceB, ttl)  → extrinsic wiring
    plug_disconnect(slot, face)            → unwire
    reroute(slot, fault)                   → Ω shape substitution

  Smart Folder (store.py):
    store.ingest_file(src)              → IngestResult (auto topology scan)
    store.delete_file(path, mode)       → soft | ghost | hard
    store.export_file(path)             → export from blob
    store.reconstruct_from_coord_wallet(wallet)  → reconstruct
    store.list_files()                  → active files only
    store.status()                      → workspace status

  C API (pogls_bond.h):
    pogls_fibo_addr(seed)               → uint64_t
    pogls_make_piece(origin_seed, axis)  → PoglsPiece (25B)
    pogls_bond_key(&piece)              → uint64_t
    pogls_bond_verify(&a, &b)           → PoglsBond {bond_key, valid}
    pogls_seed_from_fp(topology_fp)     → uint64_t
    pogls_plug_connect(a,faceA, b,faceB, ttl)  → extrinsic plug
    pogls_plug_disconnect(slot, face)           → disconnect
    pogls_reroute(&slot, fault)                 → Ω substitution

  Data flow:
    topology_fp (Python) → seed_from_fp() → seed
    seed → make_piece() → PoglsPiece {geo_key, shape, bond_L, bond_R}
    bond_key = bond_L ^ bond_R  (intrinsic, self-enforcing)
""")

print(f"\n{'='*60}")
print(f"  ALL SYSTEMS GO — สั่งรันอะไรก็เลือกข้างบน")
print(f"{'='*60}")
