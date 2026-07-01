# DEPRECATED — goldberg_sid.h (Sphere Projection)

ย้ายมาที่นี่ 30 June 2026 — ดูรายละเอียดเต็มใน AGENTS.md

## Bugs ที่พบ

| # | Bug | รายละเอียด |
|---|---|---|
| 1 | **Face 0 ≡ Face 3 duplicate** | `_goldberg_face_verts[0]` = `{0,8,4,13,12}` และ `[3]` = `{0,12,13,4,8}` — vertex set เดียวกัน, sphere หาย 1 pentagon (mapping hole) |
| 2 | **Lerp + normalize drift** | `goldberg_from_thcoord()` ใช้ lerp ระหว่างจุดบน sphere → chord ใน interior → normalize กลับ → จุด t=0.5 จม — error สะสมตาม recursive subdivision |
| 3 | **Sector collapse (60/1728)** | `side = sector/5` วิ่งได้ถึง 57, มีแค่ `if(side==0)/else` → 96.5% node_ids degenerate (1668/1728 mapping ซ้ำ) |

## ทำไมไม่แก้

- `collection/goldberg_sid.h` ไม่ถูก include โดย runner หรือ collection code ใดๆ — dead code โดยสมบูรณ์
- `runner/goldberg_sid.h` มี implementation ต่างคนละแบบ (tangent-plane projection, no lerp, no face table) — ไม่มี 3 bug นี้
- Dodecahedron pentagon = **gate/layer boundary**, ไม่ใช่ data container — precision ที่ pentagon level ไม่ critical
- Data จริงอยู่ที่ tri/hex subdivision (flat local patch) — ไม่มี curvature, topology metric `th_steps()` ใน `tri_hex_tess.h` แม่นยำและเร็วกว่า sphere approximation มาก
- Sphere/cylinder projection ไม่จำเป็นกับ architecture จริงของระบบ

เก็บไว้เผื่อวันหนึ่งมี Use Case ต้องการ continuous spherical distance metric จริงๆ
