"""Test LetterCube — 24-pair face:face bond"""
import ctypes, os

dll = ctypes.CDLL(os.path.join('.', 'geofield_pipeline.dll'))
LC_BUF = 76

# Init
buf = (ctypes.c_uint8 * LC_BUF)()
dll.geofield_lc_init(buf)
print("Init: OK")

# Assign 6 lanes with complementary pairs (0↔12, 1↔13, 2↔14)
for lane, pair, angle in [(0,0,0),(1,12,0),(2,1,1),(3,13,1),(4,2,2),(5,14,2)]:
    dll.geofield_lc_assign(buf, lane, pair, angle)
print("Assigned 6 lanes")

# Bond complementary pairs
r01 = dll.geofield_lc_bond(buf, 0, 1)
r23 = dll.geofield_lc_bond(buf, 2, 3)
r45 = dll.geofield_lc_bond(buf, 4, 5)
print(f"Bond 0<->1: {'OK' if r01 else 'FAIL'} (pair 0 <-> 12)")
print(f"Bond 2<->3: {'OK' if r23 else 'FAIL'} (pair 1 <-> 13)")
print(f"Bond 4<->5: {'OK' if r45 else 'FAIL'} (pair 2 <-> 14)")

# Assemble (lock all pending)
r = dll.geofield_lc_assemble(buf)
print(f"Assemble: {'OK' if r else 'FAIL'}")

# Print lane states
states = ['FREE', 'PEND', 'LOCK']
for i in range(6):
    pair = dll.geofield_lc_pair_id(buf, i)
    state = dll.geofield_lc_bond_state(buf, i)
    bonded = dll.geofield_lc_bonded_to(buf, i)
    print(f"  Lane {i}: pair={pair} state={states[state]} bonded_to={bonded}")

n_locked = dll.geofield_lc_n_locked(buf)
print(f"Locked bonds: {n_locked}")
print(f"Verify: {'PASS' if dll.geofield_lc_verify(buf) else 'FAIL'}")

# Serialize/deserialize roundtrip
buf2 = (ctypes.c_uint8 * LC_BUF)()
for i in range(LC_BUF):
    buf2[i] = buf[i]
v2 = dll.geofield_lc_verify(buf2)
print(f"Serialize roundtrip verify: {'PASS' if v2 else 'FAIL'}")
