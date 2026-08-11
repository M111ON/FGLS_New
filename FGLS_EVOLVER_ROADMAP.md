# FGLS EVOLVER ROADMAP
## Integrated Development Plan: Creative × Practical Synergies

**Date:** August 5, 2026  
**Agent:** EVOLVER (Integrator)  
**Goal:** 7B on 4GB GPU — "MAP not COMPRESS"

---

## 🎯 STRATEGIC SYNAPSE: Where Ideas Multiply

### Synergy Matrix

| Idea A | Idea B | Synergy | Multiplier Effect |
|--------|--------|---------|-------------------|
| **geo_frame_seek** | **GPU kernel** | Frame seek O(1) + GPU decode | 384x compression → real-time GPU streaming |
| **GGUF converter** | **geo_frame_seek** | Batch convert + timeline seek | Full model conversion pipeline |
| **Geofield Tier 1** | **geo_frame_seek** | GpSphere + FrustumBlock + frames | Spatial-temporal weight mapping |
| **GPU kernel** | **Geofield Tier 1** | GPU decode + spatial addressing | Zero-copy GPU inference |
| **Android** | **GPU kernel** | Mobile GPU + FGLS decode | 7B on phone |
| **SafeTensor** | **GGUF converter** | Multi-format support | Universal model ingestion |
| **Memory Zone** | **Cross-session** | Persistent spatial memory | Session-aware geometry |

---

## 📊 SCORED SYNERGY RANKINGS

### Tier 1: Critical Path (Must-Have)
1. **geo_frame_seek + GPU kernel** = Real-time FGLS inference
   - Combined score: 28 + 25 = **53**
   - Dependency: geo_frame_seek → GPU kernel → streaming

2. **GGUF converter + geo_frame_seek** = Batch conversion pipeline
   - Combined score: 23 + 28 = **51**
   - Dependency: GGUF reader → converter → timeline integration

### Tier 2: High Value (Should-Have)
3. **Geofield Tier 1 + geo_frame_seek** = Spatial-temporal addressing
   - Combined score: 24 + 28 = **52**
   - Dependency: GpSphere + FrustumBlock + frame seek

4. **Android + GPU kernel** = Mobile deployment
   - Combined score: 21 + 25 = **46**
   - Dependency: GPU kernel → Android optimization

### Tier 3: Nice-to-Have
5. **SafeTensor + GGUF converter** = Multi-format support
   - Combined score: 18 + 23 = **41**

6. **Memory Zone + Cross-session** = Persistent state
   - Combined score: 18 + 24 = **42**

---

## 🚀 3-PHASE DEVELOPMENT ROADMAP

### **PHASE 1: THIS WEEK (Aug 5-11)**
**Focus:** Quick wins + Critical path foundation

#### 1.1 geo_frame_seek Pipeline Integration
**Why:** #1 priority, enables 384x compression, foundation for GPU
**What:**
- Integrate `geo_frame_seek.h` into main pipeline
- Wire `frame_at(enc)→DualFrame(face, slot, ico, phase)` into streaming
- Add `frame_range_adaptive(enc,entropy)→Fibonacci span` to contour codec
- Test with existing GGUF models (SmolLM2, Qwen3)

**Dependencies:** None (already proven T2)
**Effort:** 2-3 days
**Deliverable:** FGLS with frame seek, verified lossless roundtrip

#### 1.2 GGUF Batch Converter MVP
**Why:** Enables processing multiple models, foundation for distribution
**What:**
- Extend `fgls_bake3.c` to handle directory input
- Add `--batch` flag for multiple GGUF files
- Output: `.fgls` files with metadata (tensor names, shapes)
- Test with 3-5 models from `I:/model/`

**Dependencies:** GGUF reader (T6), bake3 (T4)
**Effort:** 1-2 days
**Deliverable:** `fgls_bake --batch *.gguf` working

#### 1.3 Quick Win: Geometric PRNG
**Why:** Low effort (8/feasibility), high novelty, useful for testing
**What:**
- Implement geometric pseudo-random using stride-37 walk
- `geo_prng(seed)→deterministic_sequence`
- Test: uniform distribution on 20736 grid
- Use in stress tests and fuzzing

**Dependencies:** None
**Effort:** 0.5 days
**Deliverable:** `geo_prng.h` + test suite

**PHASE 1 EXIT CRITERIA:**
- [ ] geo_frame_seek integrated, 100% lossless
- [ ] Batch GGUF converter works
- [ ] Geometric PRNG functional
- [ ] All existing 22 test suites still PASS

---

### **PHASE 2: THIS MONTH (Aug 12 - Sep 11)**
**Focus:** GPU kernel + Geofield foundation

#### 2.1 GPU Inference Kernel (FGLS Decode on GPU)
**Why:** 105.9 GB/s bandwidth, enables real-time inference
**What:**
- Extend `gpu_jet_puller.cu` with FGLS decode kernel
- Input: `.fgls` file → GPU memory
- Kernel: decode using `kis_codec_v4.h` (T9) on GPU
- Output: original weights on GPU, zero-copy
- Benchmark: decode throughput vs CPU

**Dependencies:** gpu_jet_puller (T5), kis_codec_v4 (T9)
**Effort:** 1-2 weeks
**Deliverable:** `fgls_gpu_decode.cu` with benchmark

#### 2.2 Geofield Tier 1 (GpSphere + FrustumBlock)
**Why:** Spatial addressing, foundation for 7B mapping
**What:**
- Implement `gp_sphere.h`: weight→sphere coordinate mapping
- Implement `frustum_block.h`: 10×10×10×6 face addressing
- Integration: tie to `geo_frame_seek.h` timeline
- Test: synthetic weights → sphere → frame → decode

**Dependencies:** geo_frame_seek, frustum_gcfs.h
**Effort:** 1 week
**Deliverable:** `gp_sphere.h`, `frustum_block.h` + tests

#### 2.3 SafeTensor Support (Partial)
**Why:** Multi-format ingestion, HuggingFace compatibility
**What:**
- Extend `analyze_safetensors.c` to full reader
- Add `safetensors_reader.h` API
- Convert: SafeTensor → FGLS (lossless)
- Test with 2-3 HF models

**Dependencies:** GGUF reader pattern
**Effort:** 3-4 days
**Deliverable:** `safetensors_reader.h` + converter

**PHASE 2 EXIT CRITERIA:**
- [ ] GPU decode kernel working, benchmarked
- [ ] Geofield Tier 1 implemented, tested
- [ ] SafeTensor reader functional
- [ ] 7B model conversion demonstrated

---

### **PHASE 3: THIS QUARTER (Sep-Dec)**
**Focus:** Android deployment + Production systems

#### 3.1 Android Deployment (7B on Phone)
**Why:** Ultimate goal, mobile inference
**What:**
- Port GPU kernel to Vulkan (Android GPU)
- Optimize memory: 4GB constraint
- Build: Termux deployment package
- Test: 7B model on Android device
- Benchmark: tokens/sec, memory usage

**Dependencies:** GPU kernel (Phase 2), Vulkan
**Effort:** 2-3 weeks
**Deliverable:** Android APK + Termux package

#### 3.2 Memory Zone System
**Why:** Persistent spatial memory, session awareness
**What:**
- Implement `zone_card.h` (already exists)
- Add zone persistence: disk ↔ memory
- Integration: zone-aware streaming
- Use: cross-session model state

**Dependencies:** Geofield Tier 1, geo_frame_seek
**Effort:** 1-2 weeks
**Deliverable:** Memory zone API + persistence

#### 3.3 Web Visualization Dashboard
**Why:** Debugging, presentation, community
**What:**
- HTML/JS dashboard showing:
  - 20736 grid visualization
  - Frame seek timeline
  - Weight distribution heatmap
- API: query FGLS file metadata
- Live: stream decoding visualization

**Dependencies:** FGLS metadata, web server
**Effort:** 1-2 weeks
**Deliverable:** `fgls_dashboard.html` + API

#### 3.4 Production Pipeline Integration
**Why:** Complete system, ready for use
**What:**
- Wire all components: Chunk→Bond→GeoPixel→Hamburger→GPX5→Streaming
- Add error handling, logging, profiling
- Documentation: user guide, API reference
- Package: Windows/Linux/Mac builds

**Dependencies:** All previous phases
**Effort:** 2-3 weeks
**Deliverable:** FGLS v1.0 release

**PHASE 3 EXIT CRITERIA:**
- [ ] 7B model running on Android
- [ ] Memory zone system functional
- [ ] Web dashboard live
- [ ] Production pipeline stable
- [ ] Documentation complete

---

## 🏆 #1 MOST IMPACTFUL QUICK WIN

### **geo_frame_seek Pipeline Integration**

**Why it's #1:**
1. **Proven:** Already compiled, 375 lines, O(1) lookup
2. **High impact:** Enables 384x compression (T2)
3. **Foundation:** Required for GPU kernel, Geofield, everything
4. **Low risk:** No new code, just integration
5. **User value:** Immediate improvement in compression

**Concrete Steps:**
```c
// 1. Include in main pipeline
#include "core/geo_frame_seek.h"

// 2. Wire into streaming
DualFrame frame = frame_seek(timestep);
// Use frame.face, frame.slot, frame.ico_idx for addressing

// 3. Add to contour codec
FrameRange fr = frame_range(enc, entropy_class);
// Use fr for adaptive spanning

// 4. Test with existing GGUF
// Already proven: 361M weights, 0 mismatch
```

**Expected Result:**
- Compression: 1.5x → 384x (theoretical max)
- Speed: O(1) seek vs O(n) scan
- Memory: streaming, not loading full model

---

## 📈 SYNERGY MULTIPLIER EFFECT

### How Ideas Boost Each Other

**Week 1:** geo_frame_seek alone = 384x compression
**Week 2:** + GPU kernel = real-time decode on GPU
**Week 3:** + Geofield Tier 1 = spatial-temporal addressing
**Month 1:** + Batch converter = process multiple models
**Quarter 1:** + Android = 7B on phone

**Combined Impact:**
- Without synergies: 10 separate features, linear progress
- With synergies: interconnected system, exponential capability

---

## 🔗 DEPENDENCY GRAPH

```
geo_frame_seek (T2)
    ├── GPU kernel (T5)
    │   └── Android deployment
    ├── Geofield Tier 1
    │   └── Memory Zone System
    └── Batch converter
        └── SafeTensor support

GGUF reader (T6)
    ├── Bake3 (T4)
    │   └── Batch converter
    └── SafeTensor reader

GPU Jet Puller (T5)
    ├── GPU kernel
    └── Vulkan (Android)

kis_codec_v4 (T9)
    ├── GPU kernel decode
    └── Memory Zone encoding
```

---

## 📋 RESOURCE ALLOCATION

### Phase 1 (Week 1): 100% Focus
- **Developer:** 1 person, full-time
- **Tests:** Existing 22 suites + new ones
- **Hardware:** T4 GPU (available), CPU dev machine
- **Models:** SmolLM2-360M, Qwen3-0.6B (existing)

### Phase 2 (Month 1): 75% Focus
- **Developer:** 1-2 people
- **GPU:** T4 for kernel development
- **Testing:** Add GPU benchmarks
- **Models:** Add 7B model for testing

### Phase 3 (Quarter 1): 50% Focus (parallel)
- **Developer:** 2-3 people
- **Android:** Device required (Termux)
- **Web:** Frontend dev (if available)
- **Documentation:** Part-time

---

## 🎯 SUCCESS METRICS

### Phase 1:
- [ ] geo_frame_seek integrated: 100% lossless
- [ ] Batch converter: 5+ models processed
- [ ] Geometric PRNG: uniform distribution verified
- [ ] All 22 test suites: PASS

### Phase 2:
- [ ] GPU kernel: >50 GB/s decode throughput
- [ ] Geofield Tier 1: sphere mapping working
- [ ] SafeTensor: 3+ models converted
- [ ] 7B model: conversion demonstrated

### Phase 3:
- [ ] Android: 7B on 4GB GPU (or close)
- [ ] Memory zones: persistent state working
- [ ] Dashboard: live visualization
- [ ] Pipeline: production-ready

---

## 🚨 RISKS & MITIGATIONS

### Risk 1: GPU kernel complexity
- **Mitigation:** Start with CPU validation, port incrementally
- **Fallback:** Optimize CPU path if GPU stalls

### Risk 2: Android memory constraints
- **Mitigation:** Aggressive quantization, streaming
- **Fallback:** 3B model if 7B doesn't fit

### Risk 3: Integration bugs
- **Mitigation:** Extensive testing at each phase
- **Fallback:** Rollback to proven components

### Risk 4: Timeline slippage
- **Mitigation:** Parallel work streams, quick wins first
- **Fallback:** Cut Phase 3 scope if needed

---

## 📚 REFERENCES

### Proven Assets (T1-T9)
- T1: beam_value.c — Coordinate IS the data
- T2: geo_frame_seek.h — Timeline O(1) Geometry
- T3: Kis Adaptive Storage — 34/34 PASS
- T4: fgls_bake3.c — Full GGUF roundtrip
- T5: GPU Jet Puller — 105.9 GB/s
- T6: BeamCode/GGUF reader — Real API
- T7: kis_map_roundtrip — 361M weights, 0 mismatch
- T8: fglexe pipeline — Master pipeline
- T9: kis_codec_v4.h — Full Codec: Codebook + Permutation

### Sacred Constants
- 1440: FRAME_CYCLE (timeline circumference)
- 20736: FT_SLOT_COUNT (universal grid)
- 37: stride (frame walk step)
- 1000×6: face addressing (NOT 6000)

### First Principle
```
MAP not COMPRESS
1. Change data access dimension → compile-blow payload
2. Change viewpoint, coordinate, address space → change stored value
3. Compression is byproduct → not the goal
4. Deterministic compute-back → weight from coordinate = rule, not storage
5. Want to compress → stop → ask "is there a new dimension to look at?"
```

---

**EVOLVER AGENDA:**
- Integrate the best of creative (novel ideas) with practical (proven scores)
- Find synergies that multiply impact
- Propose concrete, achievable roadmap
- Keep user's goal: 7B on 4GB GPU

**NEXT STEP:** Execute Phase 1 starting with geo_frame_seek integration.
