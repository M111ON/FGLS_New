# Session Notes — July 24, 2026

## Geometric Distribution Analysis (GGUF → Geometry)

### Key Discovery
Stride-12-gon walk maps GGUF weights to 1440-slot timeline.
**Each model has unique geometric fingerprint** — face distribution identifies model type.

### Results (5 models tested)

| Model | Size | Face Signature | Utilization |
|-------|------|----------------|-------------|
| Qwen2.5-0.5B-Q8_0 | 644MB | F3:42% F4:45% | 57.6% |
| SmolLM2-360M-Q8_0 | 368MB | F3:38% F4:50% | 54.3% |
| Kokoro-Q8 (TTS) | 197MB | F5:40% F6:23% | 77.4% |
| Moondream2-mmproj-f16 | 868MB | F6:54% F5:27% F7:16% | 75.3% |
| SD Autoencoder-BF16 | 160MB | F6:41% F5:31% F7:18% | 69.6% |
| Qwen3-4B-2507 (multimodal) | 2.4GB | F6:38% F7:31% F5:18% | 93.8% |

### Patterns
- **Text LLM**: Face 3+4 (57-58% utilization)
- **Vision/Audio**: Face 5+6+7 (69-77% utilization)
- **Multimodal**: Face 4-8 (93.8% utilization)
- **Phase distribution**: Always uniform ~8.3% — stride walk quality metric
- **Clustering**: Near 0% — weights don't land on adjacent slots

### Architecture Insights

1. **Geometry = parallel layer to GGUF, not replacement**
   - GGUF stores VALUES, geometry stores POSITIONS
   - Same data → same position always
   - Q4 vs Q8 = different values, same position

2. **GeoStore pattern**: Bake once (10s) → Query O(1) → Switch models instantly
   - Had this working before, stopped trying to escape GGUF
   - Now understanding: geometry complements GGUF

3. **Starter Kit concept**: Pack multiple models into one file by face regions
   - Different model types → different faces → no overlap
   - On-demand loading via mmap

4. **Distribution**: Single reader.h header (like stb_image), no dependency

5. **Compression = wrong direction**: Mapping > Compression
   - Low utilization = space for activation patterns, not wasted space

### Tools Created
- `geo_distribution.c` — Analyze GGUF weights → geometric distribution
- `geo_dist_big.c` — Same, for files >2GB (uses _ftelli64)

### Reference
- `core/geo_store_reader.h` — Single-header GeoStore reader
- `docs/pipeline_concept.md` — Pipeline architecture doc
- `beam_addressing/` — Wave/dual square prototypes
