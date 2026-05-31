# Session Handoff — May 30, 2026
## MCP + ZoneCard + CoreCard — Context-Free AI State Bridge

---

## 1. สิ่งที่สร้างวันนี้

### mcp_server.py (v1)
- FastAPI + SSE, 5 tools: `get_zone_card`, `resolve_route`, `get_fusion_peer`, `store_decision`, `store_zone_card`
- Rule cache built-in (46 rules), persist JSON

### mcp_server_v2.py (v2 — production)
- CoreCard universal schema
- `push(delta, base_version)` → atomic apply / version++ / reject 409
- Event log append-only DAG (version/parent_version/author/timestamp)
- Deck namespace (`{deck}:{zone}:{shape}:{model}`)
- pull(since_version, deck) — delta stream
- Cross-deck adapter (zonecard-v2 ↔ intentcard-v1)
- Fusion table auto-update on push
- Persist: cards.json / events.json / decks.json / exact_cache.json / fusion_table.json

### core_card.py
- CoreCard dataclass (entropy/stability/locality/fingerprint/version/parent_version/author/timestamp/card_schema_version)
- `extract_local(weights)` → CoreCard จาก weight matrix
- `extract_cloud(logits, embedding)` → CoreCard จาก cloud model output
- Cross-deck adapters: zonecard-v2 → intentcard-v1 และ reverse
- `adapt_card(card, target_schema)` dispatcher

### route_cache.py (patched)
- Priority conflict fix: locality cascade 87/86 → 91/90
- `_validate_priorities()` — warn on duplicate priority ใน constructor

### locality_error_bench.py
- Benchmark locality vs reconstruction error (Q4/Q8/F16)
- Cross-model comparison
- Per-zone precision recommendation
- f16 overflow fix (cast to f32 ก่อนทุก operation)

---

## 2. ผลการทดสอบ

### Token savings
| metric | value |
|--------|-------|
| Raw geometry | ~997 tok/zone |
| ZoneCard | ~21 tok/zone (97.9%) |
| RouteRuleCache 80% hit | 99.6% total reduction |
| 10K req/day | ~10M tok → ~43K tok |

### Routing accuracy
| test | result |
|------|--------|
| 46 rules coverage | 97/97 single + 18/18 sequence = 100% |
| Colab F16 (unseen models) | hits=6 misses=0 hit_rate=100% |
| SmolLM2-360M F16 | ✅ |
| Qwen2.5-0.5B F16 | ✅ |
| Qwen2.5-Coder-3B F16 | ✅ |

### Cross-env verification
- Local Windows Q4-Q8 → pass
- Colab T4 F16 (3 unseen models) → same result
- 46 rules generalize ข้าม model architecture ได้จริง

### MCP scenario test
```
2 teams, 1 shared MCP, 3 models
cards=7 events=7 decks=2 rules=46 hits=6 misses=0
Export: 584 chars — AI ภายนอกอ่าน state ครบโดยไม่ต้อง connect MCP
```

---

## 3. Architecture ปัจจุบัน

```
plan_and_resolve(coord)
  ├─ RouteRuleCache (46 rules) → 98%+ hit
  ├─ fusion probe (borrow peer decision)
  └─ LLM fallback (novel patterns only)

MCP Server v2
  ├─ CoreCard (universal schema)
  ├─ Deck namespace
  ├─ push/pull versioning (Git-style DAG)
  └─ persist JSON → upgrade path: POGLS/GPX4

CoreCard schema
  ├─ core fields (entropy/stability/locality/fingerprint/version)
  └─ extensions (deck-specific, MCP ไม่ parse)
```

---

## 4. Key insights จาก session

### MCP = Redis + routing brain
- Redis ปกติ: dumb store, client คิดเอง
- ของเรา: server resolve decision ให้เลย (46 rules built-in)

### Git-for-AI-state
- push(delta, base_version) → version conflict → reject → rebase
- Event log = commit history = DAG
- ต่างจาก Git ตรงที่มี routing brain อยู่ใน server

### Deck = namespace/schema boundary
```
deckA:zone:shape:model → {card_type, entropy, ...}   (ZoneCard)
deckB:zone:shape:model → {confidence, embedding_hash} (IntentCard)
CoreCard = universal language ระหว่าง deck
Adapter  = translator ข้าม deck
```

### Locality ≠ precision driver (key finding)

**Hypothesis เดิม:** locality สูง → error ต่ำ (geometry ปกป้อง)
**ผลจริงจาก real weights:**

```
smollm2 correlation locality↔MSE_Q4: +0.8075  ← positive ตรงข้าม!
```

**Root cause:** locality formula = zone_id/11 correlate กับ layer depth
สิ่งที่ drive MSE จริงคือ **std ของ weight ไม่ใช่ locality**

```
smollm2 blk.0  std=0.048 → Q4 safe
smollm2 blk.1  std=0.126 → Q8/F16  (กระโดด 2.6x)
smollm2 blk.5+ std=0.150 → F16

qwen_coder     std=0.024 flat → Q4 ทุก zone (60x better than smollm2)
```

**Precision rule ที่ถูกต้อง:**
```
std < 0.05  → Q4
std < 0.13  → Q8
std ≥ 0.13  → F16
```

locality ยังใช้เป็น proxy ได้ แต่ std-based accurate กว่า
benchmark ใหม่แสดง `!` เมื่อ locality กับ std แนะนำต่างกัน

### Geometry constraint = error correction
- Q4 decode error ไม่ random — bounded โดย face boundary
- locality สูง = high precision zone
- locality ต่ำ = coarse OK
- 1 canonical store → decode Q4/Q8/F16 on demand (ยังไม่ implement)

---

## 5. สิ่งที่ยังไม่ทำ (next session)

### Priority 1 — Wire POGLS backend
```
MCP JSON dict → POGLS GeoField store (.gsidx/.gsdat)
JSON I/O 500ms → GPX4 ~50ms
Fusion O(n²)  → GeoField neighborhood O(1)
```

### Priority 2 — Stress test 10x-100x
```
stress_test.py --cards 10000 --workers 8
พิสูจน์: prototype → framework จริง
```

### Priority 3 — Multi-resolution decoder
```
canonical store + BermudaGate codebook
→ decode(q_level, locality) → Q4/Q8/F16 on demand
1 store = ทุก precision ไม่ต้องแยกไฟล์
```

### Priority 4 — Human override + shadow rules
```
user override → shadow rule → รัน 100 cases → promote ถ้า win > 80%
GPT proposal: shadow rule ก่อน promote เข้า production
```

### Priority 5 — Rule drift validation
```
POST /tools/validate_rules
push test vectors → วัด % rule match หลัง model version bump
rule_version / model_version / deck_version แยกกัน
```

---

## 6. Files

| file | description |
|------|-------------|
| mcp_server_v2.py | Production MCP server |
| core_card.py | Universal CoreCard schema + extractors + adapters |
| route_cache.py | 46 rules, priority conflict fixed |
| mcp_client.py | Client helper + patch_pool() |
| locality_error_bench.py | Locality vs MSE benchmark |
| mcp_colab_pack_v2.zip | Colab-ready pack |

---

## 7. One-liner สำหรับอธิบายระบบ

> "Context-free MCP — bake geometry knowledge เป็น ZoneCard LUT  
> ให้ LLM route โดยไม่ต้อง load context  
> persist ข้าม session, sync ข้าม team, ต่างค่าย AI อ่านได้ใน 584 chars"

---

*Session: May 30, 2026 | Tests: 38/38 + Colab F16 verification PASS*
