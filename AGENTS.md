# AGENTS.md — Session Handoff

## ⚠️ ห้ามใช้ git โดยเด็ดขาด
- **ห้าม run `git` commands ใดๆ ทั้งสิ้น** ไม่ว่า git status, git add, git commit, git push, git clean, git reset, git checkout ฯลฯ
- หากจำเป็นต้องตรวจสอบประวัติไฟล์ ให้ใช้ Read tool อ่านไฟล์โดยตรงเท่านั้น
- หาก user ต้องการให้ commit หรือ push เดี๋ยว user จัดการเอง
- ข้อยกเว้น: ถ้า user สั่งให้ใช้ git โดยตรง (written in stone) เท่านั้น

## 🛑 วงจรอุบาทว์ (Fix→Crash Loop) Protocol
เมื่อเข้า loop: แก้ → crash → แก้ → crash → แก้ → crash เกิน **3 รอบติด**:
1. **หยุดทันที** — อย่าแก้ต่อ
2. **ประเมินสถานการณ์**: ถาม user ว่า "เราควรเปลี่ยนแนวทางหรือลองอะไรต่อ?"
3. **ทบทวนสมมติฐาน**: อ่านไฟล์ที่เกี่ยวข้องทั้งหมดอีกครั้ง (ไม่ใช่แค่บรรทัดที่ crash)
4. **ใช้วิธีที่ง่ายที่สุด** ที่น่าจะใช้ได้ก่อน — อย่าเพิ่ม abstraction, indirection, หรือ fallback scan ที่ซับซ้อน
5. **เมื่อไม่แน่ใจ**: ใช้ printf/fprintf debug ทีละชั้นก่อน — อย่าเดา root cause

## Pre-scan check
- ก่อนเรียก scan() ให้เรียก get_project_index() เพื่อดูสถานะ cache
- หาก cache มีข้อมูลล่าสุด ให้ใช้ cached data แทนการสแกนใหม่
- หาก cache ไม่มีข้อมูลหรือล้าสมัย ให้สแกนตามปกติ

## 🧭 Ground Rule: มองรอบก่อนพุ่ง
เมื่อติดปัญหา — หยุด มองดูว่ามี asset อะไรในโปรเจ็กต์ที่เกี่ยวข้องอยู่แล้วบ้าง
โปรเจ็กต์นี้มีของดีผ่านการทดลองมามาก แต่บางอันถูกทิ้งเพราะมีอะไรดีกว่ามาแทน หรือยังไม่เจอเคสเหมาะ
**อย่า hardcode/scan ใหม่ ถ้ามีของที่ใช้ได้อยู่แล้ว**

---

## 🧪 Session June 18 — CORRECTION: Backend Reads tensor->data Per-Decode (Not Cached at Init)

### tl;dr
**The CPU backend reads `tensor->data` on EVERY `llama_decode` call, NOT cached at `llama_init_from_model`.** 
- Swapping tensor->data **after** context creation, then calling `llama_decode` → logits change (proven: 151935/151936 logits differ, max_diff=0.358)
- `llama_free(ctx)` + new `llama_init_from_model` somehow resets state (old test with 2 contexts showed no difference)
- **Correct flow: swap between decodes, same context**

### What Changed from Earlier Claim
Earlier AGENTS.md entry claimed "swap before context works, swap after = zero effect." This was WRONG.
- The old proof test (test_swap_debug.c) found only 13/291 tensors via model struct scan
- Old test created 2 separate contexts (baseline + test) → `llama_free` + new context masked the swap effect
- New test (test_swap_all.c) uses **same context, 2 decodes** — swap BETWEEN decodes works definitively

### Key Implementations Now Working
1. **`gguf_idx_open`** (existing in `gguf_index.h`) — reads tensor names from GGUF file. Replaces hardcoded 291 name list.
2. **Model struct scan** (`scan_region_for_tensors`) — scans model struct (64KB) + layers heap allocation for tensor pointers matching GGUF names. Uses VirtualQuery for memory safety (no SEH, no process-wide scan).
3. **291/291 tensors found** — both direct model tensors (3) + all layer tensors (288) via layers-pointer discovery.
4. **Per-decode swap proven** — set tensor->data to copies (with 1-byte sentinel flip) → decode sees changed data.

### Implications for SID
- No need to intercept context creation
- SID swap happens BETWEEN `llama_decode` calls
- Flow: decode(token_n) → swap tensor->data to SID cache → decode(token_n+1) → swap back
- Works with any backend (CPU, GPU, Vulkan) — backend reads tensor->data per-decode

---

## 🔮 SID Vision (from user, June 18)

### Shadow Clone + Venom Philosophy
- **Shadow Clone**: independent execution, expire → transfer experience. Not parallel dummy.
- **Venom**: inject, take over, carry forward, enhance next target. Zero waste, evolutionary.
- **No reverse engineering**: chasing version updates is unsustainable.

### Next Phase: SID → Tensor Memory → Topology
```
input (screen/text/event)
    ↓
encode → SID coordinate (face, spoke, slot)
    ↓
map   → tensor region (layer, head, channel)
    ↓
bond  → neighbor tensors via topology edges
```

### Bond Discovery (user's findings with line primitives)
Lines, length, angle, intersection can be:
- Projected, offset, reconstructed
- Origin drift/shift along path
- Parameterized lines = **routers** without needing actual geometry

**Bond is lower-level than geometry** — it's a level below geometric forms, acting as a pure topological primitive.

---

## Time Travel (June 15 — Delta Ring + Rewind/Fast-Forward)

### Core Files
- **`runner/sid_delta_ring.h`** — Circular buffer journal of SID swap operations. Each entry records `(ft_idx, tensor_ptr, orig_data, sid_data, size)`. Supports checkpoint names (up to 64), automatic tail eviction on overflow.
- **`runner/sid_timetravel.h`** — Orchestrator wrapping delta ring. `sid_timetravel_before_decode()` pushes swap entries into ring before each `sid_swap_apply()`. `sid_timetravel_after_decode()` processes pending checkpoint/rewind/ffwd commands.

### How It Works
- Before every `sid_swap_apply()`, the delta ring records all tensor pointers being swapped: the original data pointer AND the SID cache pointer. **No data copying** — just pointer tracking.
- After every `sid_swap_restore()`, pending time travel commands are processed:
  - **Rewind**: walks the ring backward from tail to checkpoint, restores `orig_data` pointer in each tensor struct. Undoes all SID injections back to the checkpoint.
  - **Fast-forward**: walks forward from checkpoint to head, re-applies `sid_data` pointers. Re-does all SID injections.
  - **Checkpoint**: marks current ring position with a name.
  - **Branch**: copies ring state up to checkpoint into a new ring.
- Ring capacity = 1024 entries. Older entries evicted from tail when full.

### CLI Flags & Chat Commands
```
--sid-checkpoint NAME    /checkpoint NAME     Save checkpoint
--sid-rewind NAME        /rewind NAME         Rewind (undo swaps)
--sid-ff NAME            /ff NAME             Fast-forward (redo swaps)
--sid-branch NAME        /branch NAME         Branch state
                         /tt                  Print time travel state
```

### Key Design Decision
- **Pointer-only journaling**: no data copies. Rewind = pointer restore in reverse; Fast-forward = pointer re-apply forward. Copy-on-write branching with different SID data is future work.
- **Chronological ring**: each swap cycle produces one entry per tensor. 170 tensors × 2 cycles/token = 340 entries/token. Capacity 1024 entries ≈ 3 tokens of history. Bump `SID_DELTA_MAX_ENTRIES` for longer history.

---

## 🌡️ Bond Prediction (June 15 — Cardioid Express + Metatron Route)

### What It Does
When `--bond` is passed, the runner builds a **bond graph** over all 290 discovered tensors, then computes a **hotness score** (0.0 cold → 1.0 hot) for each tensor using:

1. **Cardioid Express** — Maps each layer to a cardioid geometry position `pos = layer * 720 / n_layers`. Applies `r(θ)=a(1+cosθ)` Q8 fixed-point gate: inside cardioid = express (hot=1.0), outside = warm (0.5), cusp near θ=π = cold (0.1). Signal byte `(layer*17+42)&0xFF` as deterministic per-layer data value.

2. **Metatron Route Topology** — Maps each weight tensor to a Metatron face (0..11) based on slot type + ring, then discovers:
   - **META_ORB**: same face, adjacent slot (= same type, consecutive layers)
   - **META_CHIRAL**: opposite face (±6), same slot (= same type, opposite ring)
   - **META_CROSS**: inter-ring bijection via CROSS_MAP LUT

3. **Hotness Propagation** — 3-pass diffusion: each tensor's hotness spreads to bonded neighbors (weighted by bond strength). After propagation, hotness reflects both cardioid position AND topological connectivity.

### CLI Flag
- `--bond`: Enables bond discovery, cardioid scoring, and Metatron topology. When combined with `--sid-face N`, cold tensors (hotness < 0.3) are skipped from SID swaps.

### Output
```
[bond] 2320 total bonds:
  LAYER_SLOT:    217
  INTRA_LAYER:   672
  CARDIOID:      889
  META_ORB:      270
  META_CHIRAL:   144
  META_CROSS:    128
[predict] hot=32.4% warm=57.2% cold=10.3%
[predict] top-5 hottest:
  1.00  blk.0.attn_norm.weight
  1.00  blk.0.ffn_norm.weight
  1.00  blk.1.attn_norm.weight
  1.00  blk.1.ffn_norm.weight
  1.00  blk.2.attn_norm.weight
[sid] bond filter active: will skip cold tensors (hotness < 0.3)
[sid] 217 / 290 tensors will be swapped per decode (bond filter skipped 30 cold)
```

### Relevant Files
- **`runner/bond_discovery.h`**: All bond types — static (LAYER_SLOT, INTRA_LAYER), cardioid express phase, and Metatron topology (orbital, chiral, cross). Also: `bond_predict_hotness()`, `bond_predict_print()`, heap-allocated BondGraph with `bond_graph_init()`/`bond_graph_free()`.
- **`runner/llama_pogls_runner_sid_v2.c`**: `--bond` flag integration at line ~382. Computes n_layers from found tensor names, passes to bond discovery, stores bond_hotness array, applies cold filter in SID swap setup (line ~510).

### Notes
- Cardioid bonds limited to `|layer_diff| <= 1` to avoid N² explosion.
- BondGraph is heap-allocated with capacity `n_found * 8` (up to 32768).
- BOND_METATRON_HUB defined in enum but not currently discovered (too noisy).
- Hotness propagation: 3 passes, 0.7 self-weight + 0.3 × bond_weight × neighbor hotness.
