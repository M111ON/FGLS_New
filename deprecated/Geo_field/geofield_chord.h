/*
 * geofield_chord.h — Chord-Based Random Access
 * ═════════════════════════════════════════════
 *
 * CONCEPT (music sheet analogy):
 *   data   = notes
 *   chord  = key signature → กด 1 chord → point หลาย slot พร้อมกัน
 *   strum  = read (access all pointed slots simultaneously)
 *   field  = guitar fretboard → transposable (change key = change offset)
 *
 * CHORD = MULTIPLE POINTER:
 *   chord มี root + quality + extensions
 *   → map ไปยัง (base_addr, offset_set[], direction_flags)
 *   → point หลาย DiamondBlock พร้อมกัน = parallel random access
 *
 *   guitar analogy:
 *     กด chord shape → หลาย string ดังพร้อมกัน
 *     capo (transpose) = shift base_addr ทั้ง chord
 *     #/b = offset direction (+1/-1 semitone = ±slot offset)
 *
 * CHORD STRUCTURE:
 *   root       = base address (GpAddr tile_id)
 *   quality    = major/minor → offset pattern
 *     major: intervals [0, 4, 7]   (root, major3rd, perfect5th)
 *     minor: intervals [0, 3, 7]   (root, minor3rd, perfect5th)
 *   extension  = 7th/9th → เพิ่ม pointer อีก 1-2 ตัว
 *   accidental = #/b → ±1 offset applied to all intervals
 *
 * RANDOM ACCESS:
 *   chord_press(chord) → resolves to N GpAddr (N = chord note count)
 *   chord_strum(chord, blk) → read N slots พร้อมกัน → return N chunks
 *   O(1) per slot — pure geometric lookup, no scan
 *
 * TRANSPOSE:
 *   chord_transpose(chord, semitones) → shift root by semitone count
 *   semitone = 1 unit in spoke/coset space (TODO: define mapping)
 *   key change = access different region without re-specifying full addr
 *
 * FIELD MAPPING (TODO: define semitone → GpAddr offset):
 *   Options:
 *     A) semitone = ±1 spoke  (6 semitones = full rotation)
 *     B) semitone = ±1 coset  (9 semitones = full coset cycle)
 *     C) semitone = ±1 tile_id (linear — less geometric)
 *   → choice กระทบ chord_transpose() ทั้งหมด
 *
 * CHORD COLLISION:
 *   สอง chord ชน same slot → last-write wins (simple)
 *   หรือ merge (XOR) → ยังไม่ตัดสินใจ
 *
 * ═════════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include "format/geofield_header.h"    /* chord_root, chord_flags fields */
#include "geo_goldberg_sphere.h"       /* GpAddr                         */

/* ── Chord quality ──────────────────────────────────────────── */
#define CHORD_MAJOR     0u   /* intervals: 0, 4, 7                       */
#define CHORD_MINOR     1u   /* intervals: 0, 3, 7                       */
#define CHORD_DOM7      2u   /* intervals: 0, 4, 7, 10  (dominant 7th)  */
#define CHORD_MAJ7      3u   /* intervals: 0, 4, 7, 11  (major 7th)     */
#define CHORD_MIN7      4u   /* intervals: 0, 3, 7, 10  (minor 7th)     */

/* Max notes per chord (extensions up to 4 notes) */
#define CHORD_MAX_NOTES 4u

/* ── Accidental flags (chord_flags in GeoFieldHeader) ────────── */
#define CHORD_FLAG_SHARP    (1u << 0)   /* # → +1 offset all intervals  */
#define CHORD_FLAG_FLAT     (1u << 1)   /* b → -1 offset all intervals  */
#define CHORD_FLAG_EXT7     (1u << 2)   /* add 7th extension            */
#define CHORD_FLAG_EXT9     (1u << 3)   /* add 9th extension            */

/* ── Chord descriptor ───────────────────────────────────────── */
typedef struct {
    uint32_t root_tile;     /* base tile_id (root note address)         */
    uint8_t  quality;       /* CHORD_MAJOR / MINOR / DOM7 / etc.        */
    uint8_t  flags;         /* CHORD_FLAG_* accidentals + extensions    */
    uint8_t  transpose;     /* semitone offset from root (0 = no capo)  */
    uint8_t  _pad;
} GeoChord;

/* ── Resolved chord: N GpAddr pointers ─────────────────────── */
typedef struct {
    GpAddr  slots[CHORD_MAX_NOTES];   /* resolved addresses              */
    uint8_t count;                    /* actual note count (2..4)        */
} GeoChordResolved;

/* ── Interval tables ────────────────────────────────────────── */
static const uint8_t _chord_intervals[5][4] = {
    {0, 4, 7,  0},   /* MAJOR  — 3 notes, pad 0 */
    {0, 3, 7,  0},   /* MINOR  — 3 notes, pad 0 */
    {0, 4, 7, 10},   /* DOM7   — 4 notes         */
    {0, 4, 7, 11},   /* MAJ7   — 4 notes         */
    {0, 3, 7, 10},   /* MIN7   — 4 notes         */
};
static const uint8_t _chord_note_count[5] = {3, 3, 4, 4, 4};

/* ── TODO: semitone → tile_id offset ───────────────────────── */
/*
 * _chord_semitone_offset — convert semitone to tile_id delta
 * PENDING: decide mapping type A/B/C (see header comment)
 * Placeholder: linear +semitone for now
 */
static inline int32_t _chord_semitone_offset(uint8_t semitone) {
    /* TODO: replace with geometric mapping (spoke or coset) */
    return (int32_t)semitone;
}

/* ── Press chord → resolve to GpAddr array ──────────────────── */
/*
 * geo_chord_press — resolve GeoChord to N GpAddr slots
 * All O(1) — pure table lookup + arithmetic, no scan
 */
static inline GeoChordResolved geo_chord_press(const GeoChord *ch,
                                                uint8_t gp_level,
                                                uint8_t dim)
{
    GeoChordResolved r;
    uint8_t q = (ch->quality < 5) ? ch->quality : 0;
    r.count   = _chord_note_count[q];

    uint32_t face_max = gp_face_count(gp_level);

    /* accidental offset */
    int32_t acc = 0;
    if (ch->flags & CHORD_FLAG_SHARP) acc = +1;
    if (ch->flags & CHORD_FLAG_FLAT)  acc = -1;

    /* transpose offset */
    int32_t trans = _chord_semitone_offset(ch->transpose);

    for (uint8_t i = 0; i < r.count; i++) {
        int32_t interval = (int32_t)_chord_intervals[q][i];
        int32_t tile = (int32_t)ch->root_tile + interval + acc + trans;
        /* wrap within face_max */
        tile = ((tile % (int32_t)face_max) + (int32_t)face_max)
               % (int32_t)face_max;
        r.slots[i].tile_id = (uint32_t)tile;
        r.slots[i].dim     = dim;
    }
    return r;
}

/* ── Transpose chord ────────────────────────────────────────── */
/*
 * geo_chord_transpose — shift chord root by semitones
 * Returns new chord (original unchanged)
 */
static inline GeoChord geo_chord_transpose(GeoChord ch, int8_t semitones) {
    /* TODO: use geometric offset once semitone mapping defined */
    ch.transpose = (uint8_t)((int16_t)ch.transpose + semitones);
    return ch;
}

/* ── Strum: read N slots from FrustumBlock ──────────────────── */
/*
 * geo_chord_strum — parallel read of all chord slots
 * out_chunks[i] = pointer to 64B data for slot i (NULL if not written)
 * Returns count of non-NULL chunks read
 *
 * TODO: needs FrustumBlock + gp_level context
 * Signature is a placeholder — refine when GeoField API is clearer
 */
/* static inline uint8_t geo_chord_strum(
 *     const GeoChordResolved *r,
 *     uint8_t gp_level,
 *     const FrustumBlock *blk,
 *     const uint8_t *out_chunks[CHORD_MAX_NOTES]);
 * TODO: implement after chord_press verified */

/* ── Build chord from GeoFieldHeader reserved fields ─────────── */
/*
 * geo_chord_from_header — extract chord descriptor from header
 * Uses chord_root (tile base) + chord_flags (quality + accidentals)
 * chord_root=0 + chord_flags=0 → identity chord (no random access override)
 */
static inline GeoChord geo_chord_from_header(const GeoFieldHeader *h) {
    GeoChord ch;
    ch.root_tile = h->chord_root;         /* 8-bit → low 256 tiles       */
    ch.quality   = h->chord_flags & 0x07u;
    ch.flags     = (h->chord_flags >> 3) & 0x0Fu;
    ch.transpose = 0;
    ch._pad      = 0;
    return ch;
}
