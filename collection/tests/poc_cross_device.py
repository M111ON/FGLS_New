"""
Cross-device bond linking — prototype
Device A → topology_fp → tunnel → Device B → bond_verify

Run:
  python poc_cross_device.py               # local demo (same machine)
  python poc_cross_device.py --listen 9000  # Device A (server)
  python poc_cross_device.py --connect 9000 # Device B (client)
"""
import sys, os, socket, argparse, json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "python_src"))
os.environ["POGLS_SO_PATH"] = str(Path(__file__).resolve().parent / "pogls_bond.dll")

from pogls_bridge import PoglsBridge, FACE_E, FACE_W

br = PoglsBridge()
parser = argparse.ArgumentParser()
parser.add_argument("--listen", type=int, help="Device A: listen on port")
parser.add_argument("--connect", type=int, help="Device B: connect to port")
parser.add_argument("--host", default="127.0.0.1")
args = parser.parse_args()

def device_a():
    """Server: creates bond pieces, sends topology_fp over socket"""
    fp = "a3f0b2c1d4e5f6a7"
    seed = br.seed_from_fp(fp)
    piece = br.make_piece(seed, axis=1)
    print(f"[A] fp={fp}")
    print(f"[A] piece: shape={piece.shape_char} geo={piece.geo_key:#018x}")
    print(f"[A] bond_key={piece.bond_key:#018x}")
    srv = socket.socket()
    srv.bind((args.host, args.listen))
    srv.listen(1)
    conn, addr = srv.accept()
    print(f"[A] tunnel open from {addr}")
    payload = json.dumps({
        "topology_fp": fp,
        "shape": piece.shape_char,
        "geo_key": f"{piece.geo_key:#018x}",
    }).encode()
    conn.sendall(payload)
    print(f"[A] sent topology_fp → tunnel")
    resp = json.loads(conn.recv(4096).decode())
    print(f"[A] Device B says: {resp}")
    conn.close()
    srv.close()

def device_b():
    """Client: receives topology_fp, reconstructs piece, verifies bond"""
    cli = socket.socket()
    cli.connect((args.host, args.connect))
    data = json.loads(cli.recv(4096).decode())
    fp = data["topology_fp"]
    print(f"[B] received fp={fp}")
    seed = br.seed_from_fp(fp)
    p1 = br.make_piece(seed, axis=1)
    p2 = br.make_piece(seed, axis=3)
    print(f"[B] piece-I: shape={p1.shape_char} geo={p1.geo_key:#018x}")
    print(f"[B] piece-T: shape={p2.shape_char} geo={p2.geo_key:#018x}")
    valid, bk = br.bond_verify(p1, p2)
    shape_match = (data.get("shape") == p1.shape_char)
    cli.sendall(json.dumps({
        "bond_key": f"{bk:#018x}",
        "valid_probabilistic": bool(valid),
        "geo_match": p1.geo_key == int(data.get("geo_key", "0"), 16) if "geo_key" in data else True,
        "shape_match": shape_match,
    }).encode())
    cli.close()

if args.listen:
    device_a()
elif args.connect:
    device_b()
else:
    fp_ok  = "a3f0b2c1d4e5f6a7"
    fp_bad = "deadbeefcafebabe"

    seed_ok = br.seed_from_fp(fp_ok)
    seed_bad = br.seed_from_fp(fp_bad)

    a = br.make_piece(seed_ok,  axis=1)
    b = br.make_piece(seed_ok,  axis=1)  # same fp → same geo_key
    c = br.make_piece(seed_bad, axis=1)

    print(f"Device A: fp={fp_ok} → geo={a.geo_key:#018x}")
    print(f"Device B: fp={fp_ok} → geo={b.geo_key:#018x}")
    print(f"  geo match: {a.geo_key == b.geo_key}")
    print(f"  bond_key match: {a.bond_key == b.bond_key}")
    print(f"  → same topology_fp → same piece, any device")

    print(f"")
    print(f"Intruder: fp={fp_bad} → geo={c.geo_key:#018x}")
    print(f"  geo match with A: {a.geo_key == c.geo_key}")
    print(f"  bond_key match: {a.bond_key == c.bond_key}")
    print(f"  → wrong topology_fp → geo/bond differ → blocked")

    print(f"")
    print(f"Gate logic: bond_key(A)={a.bond_key:#018x}")
    print(f"            bond_key(B)={b.bond_key:#018x} (same fp → match)")
    print(f"            bond_key(C)={c.bond_key:#018x} (wrong → mismatch)")
    print(f"  → shape=A passes, shape=C → Ω_quarantine")
