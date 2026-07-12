# Bermuda Fingerprint — Visual ID Spec v1 (Draft)

## Core Concept

Bermuda `.bpack` → unique visual PNG using **LetterCube 6-face structure**.
A-Z (WorldA) / a-z (WorldB) pairs on 1×1×1 unit cube → hexagonal cylinder layout.

## Visual Layout

### Hexagonal Cylinder (unfolded continuous strip)

```
  Face0  [A . . Z]   ← WorldA (upper)
  Face1  [a . . z]   ← WorldB (lower)
  Face2  [A . . Z]   ← WorldA (upper)
  Face3  [a . . z]   ← WorldB (lower)
  Face4  [A . . Z]   ← WorldA (upper)
  Face5  [a . . z]   ← WorldB (lower)
```

6 faces in a ring = **continuous**, no seam.
Alternative: 4×3 grid, or wrapped hexagonal prism.

### 26 letter slots per face

Each face has 26 positions (A-Z or a-z).
Total: 6 × 26 = **156 cells**.
Per file: `hash(path) % 156` → (face_id, letter_slot).
Empty cells → baseline dark.

## Data Encoding

### Cell content per file

| Field | Size | Source |
|-------|------|--------|
| Letter slot (A-Z/a-z) | 5 bits | hash(path) → position |
| Colour hue | 8 bits | xxh64 0:7 |
| Colour lightness | 8 bits | xxh64 8:15 |
| Cell size | 8 bits | log(file size), normalized |
| Face ID (0-5) | 3 bits | from position mapping |
| **Total per file** | **~32 bits** | >156 files unique |

### Visibility

Human sees: colour blocks (warm=cool, size=large/small).
No text rendering — pure visual.

## LetterCube Integration

geo_letter_cube.h provides:

- `LC_PAIRS` = 26, `LC_FACES` = 6
- `lc_make_pair(idx)` → unique LetterPair (upper=lower)
- `lc_pair_valid(p)` → validate
- 78-step closure walk → can be used for animation/ordering

The fingerprint image IS a LetterCube visualisation:
- Each face rendered as a horizontal bar
- Each letter slot as a coloured cell
- Pairing visible as vertical alignment between Face0/Face1

## Render Output

- PNG 512×512 (or scalable)
- Dark background (#10141a)
- Cell colours: HSL derived from xxh64
- Mirror symmetry optional (adds aesthetic but reduces info density)

## Implementation Plan

1. **Fingerprint generator** (Python, bermuda/fingerprint.py):
   - `BermudaPack.fingerprint()` → reads manifest → renders hexagonal PNG
   
2. **CLI**:
   ```bash
   python -m bermuda fingerprint build.bpack -o fingerprint.png
   ```

3. **LetterCube backend** (optional):
   - Use `geo_letter_cube.h` C code via ctypes/CFFI for real LC closure walk
   - Fallback: pure Python reimplementation

## Security

- **One-way**: fingerprint from content hash — cannot reconstruct files from image
- **Deterministic**: same pack → same fingerprint
- **Tamper-evident**: 1-bit file change → completely different pattern

---

See also: `core/core/geo_letter_cube.h`, `runner/bermuda/`
