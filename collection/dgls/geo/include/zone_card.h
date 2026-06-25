#pragma once
/*
 * zone_card.h — ZoneCard v3
 * ============================================================
 * v1:  10B  id/type/entropy/pattern/neighbor
 * v2:  12B  + locality/stability
 * v3:  28B  + zone_id/confidence/flags/parent_id/ttl/timestamp
 *
 * BACKWARD COMPAT: v2 (12B/20B) and v1 (10B/18B) readable via
 * zone_card_read() with version detection.
 * ============================================================
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Card types ───────────────────────────────────────────── */
#define CARD_SPARSE    0
#define CARD_BATCH     1
#define CARD_LZ        2

/* ── Sentinel ─────────────────────────────────────────────── */
#define NO_NEIGHBOR    0xFFFF
#define NO_PARENT      0xFFFF
#define NO_ZONE        0xFFFF

/* ── flags bits (uint8) ───────────────────────────────────── */
#define CARD_FLAG_EXPIRED     (1u << 0)   /* ttl elapsed             */
#define CARD_FLAG_HOT_PATH    (1u << 1)   /* pentagon axis node      */
#define CARD_FLAG_GPU_HINT    (1u << 2)   /* prefer GPU dispatch     */
#define CARD_FLAG_CHAINED     (1u << 3)   /* part of session chain   */
#define CARD_FLAG_GLOBE_B     (1u << 4)   /* routed via Globe B      */
/* bits 5-7 reserved for future use */

/* ── Version tags (first byte of serialised stream) ──────── */
#define CARD_VERSION_1   0x01
#define CARD_VERSION_2   0x02
#define CARD_VERSION_3   0x03

/* ── v2 core (12 bytes) — never modified ─────────────────── */
typedef struct {
    uint16_t id;              /* zone index 0-4095             */
    uint8_t  card_type;       /* SPARSE/BATCH/LZ               */
    uint8_t  entropy;         /* byte-level entropy 0-255      */
    uint16_t pattern;         /* 16-bit fingerprint            */
    uint8_t  locality;        /* similarity to neighbors 0-255 */
    uint8_t  stability;       /* row-to-row stability 0-255    */
    uint16_t neighbor_left;   /* left zone id  / NO_NEIGHBOR   */
    uint16_t neighbor_right;  /* right zone id / NO_NEIGHBOR   */
} ZoneCard;
_Static_assert(sizeof(ZoneCard) == 12, "ZoneCard v2 must be 12 bytes");

/* ── v2 extended (20 bytes) ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  card_type;
    uint8_t  entropy;
    uint16_t pattern;
    uint8_t  locality;
    uint8_t  stability;
    uint16_t neighbor_left;
    uint16_t neighbor_right;
    uint64_t hash_val;
} ZoneCardExt;
_Static_assert(sizeof(ZoneCardExt) == 20, "ZoneCardExt must be 20 bytes");

/* ── v3 routing-aware (28 bytes) ─────────────────────────── */
typedef struct {
    /* ── v2 core (12B) — layout identical ── */
    uint16_t id;              /* zone index 0-4095             */
    uint8_t  card_type;       /* SPARSE/BATCH/LZ               */
    uint8_t  entropy;         /* byte-level entropy 0-255      */
    uint16_t pattern;         /* 16-bit fingerprint            */
    uint8_t  locality;        /* similarity to neighbors 0-255 */
    uint8_t  stability;       /* row-to-row stability 0-255    */
    uint16_t neighbor_left;
    uint16_t neighbor_right;
    /* ── v3 routing extension (16B) ── */
    uint16_t zone_id;         /* geo address 0-20735 (Globe A+B) */
    uint8_t  confidence;      /* router confidence 0-255         */
    uint8_t  flags;           /* CARD_FLAG_* bitmask             */
    uint16_t parent_id;       /* session chain parent / NO_PARENT */
    uint16_t ttl;             /* ticks until stale (0=never)     */
    uint32_t ext_ptr;         /* pointer to ZoneCardMeta (0=none) */
    uint64_t hash_val;        /* FNV-1a 64-bit fingerprint        */
} ZoneCardV3;
_Static_assert(sizeof(ZoneCardV3) == 32, "ZoneCardV3 must be 32 bytes");

/* ── Serialised v3 stream: [version:u8][ZoneCardV3:32B] ── */
#define CARD_V3_STREAM_SZ  (1u + 32u)   /* 33 bytes */

/* ════════════════════════════════════════════════════════════
 * ZoneCardMeta — extensible payload pointed to by ext_ptr
 * ════════════════════════════════════════════════════════════
 *
 * Inspired by Ethereum calldata: card is the transaction header,
 * meta is the contract body. Router reads card (32B) for O(1)
 * decisions; follows ext_ptr only when detail is needed.
 *
 * Layout of a meta block:
 *   [meta_version:u8][type_id:u8][payload_len:u16][payload:N bytes]
 *
 * type_id determines how payload is decoded (open ABI).
 * ext_ptr = 0 means no metadata attached.
 *
 * Built-in type_ids:
 *   META_TASK      0x01  — task_bits + model_key hint
 *   META_TEMPLATE  0x02  — prompt template reference
 *   META_CHAIN     0x03  — model chain routing rules
 *   META_CONDITION 0x04  — conditional routing bytecode
 *   META_CUSTOM    0xFF  — user-defined free-form bytes
 * ════════════════════════════════════════════════════════════ */

#define META_VERSION_1   0x01

#define META_TASK        0x01
#define META_TEMPLATE    0x02
#define META_CHAIN       0x03
#define META_CONDITION   0x04
#define META_CUSTOM      0xFF

/* ── Task bits (META_TASK, uint16) ── */
#define TASK_CODE        (1u << 0)
#define TASK_MATH        (1u << 1)
#define TASK_CHAT        (1u << 2)
#define TASK_REASON      (1u << 3)
#define TASK_VISION      (1u << 4)
#define TASK_MULTILANG   (1u << 5)
#define TASK_SUMMARIZE   (1u << 6)
#define TASK_EMBED       (1u << 7)
/* bits 8-15 reserved for user-defined tasks */

/* ── Meta header (4 bytes) ── */
typedef struct {
    uint8_t  meta_version;   /* META_VERSION_*                    */
    uint8_t  type_id;        /* META_TASK / META_TEMPLATE / etc.  */
    uint16_t payload_len;    /* bytes that follow this header      */
} ZoneCardMetaHeader;
_Static_assert(sizeof(ZoneCardMetaHeader) == 4, "MetaHeader must be 4 bytes");

/* ── META_TASK payload (4 bytes) ── */
typedef struct {
    uint16_t task_bits;      /* TASK_* bitmask                    */
    uint8_t  model_hint;     /* preferred model slot (0=any)      */
    uint8_t  priority;       /* 0=normal 255=highest              */
} ZoneCardMetaTask;
_Static_assert(sizeof(ZoneCardMetaTask) == 4, "MetaTask must be 4 bytes");

/* ── META_TEMPLATE payload (4 bytes) ── */
typedef struct {
    uint16_t template_id;    /* prompt template registry id       */
    uint8_t  lang_hint;      /* ISO-639 index (0=any)             */
    uint8_t  reserved;
} ZoneCardMetaTemplate;
_Static_assert(sizeof(ZoneCardMetaTemplate) == 4, "MetaTemplate must be 4 bytes");

/* ── META_CONDITION bytecode ops (op:u8, operand:u8 pairs) ──
 *   0x01 IF_TASK_BIT  operand = TASK_* bit index
 *   0x02 IF_ENTROPY   operand = threshold (route if entropy > N)
 *   0x03 IF_CONF      operand = threshold (route if confidence > N)
 *   0x04 ELSE         operand = model_slot
 *   0xFF END
 */
#define MCOND_IF_TASK_BIT  0x01
#define MCOND_IF_ENTROPY   0x02
#define MCOND_IF_CONF      0x03
#define MCOND_ELSE         0x04
#define MCOND_END          0xFF

/* ── ext_ptr helpers ── */
static inline void zone_card_set_ext(ZoneCardV3 *c, uint32_t ptr) {
    if (c) c->ext_ptr = ptr;
}
static inline int zone_card_has_ext(const ZoneCardV3 *c) {
    return c && c->ext_ptr != 0;
}

/* ── Serialise META_TASK into buf (>= 8 bytes). Returns bytes written. ── */
static inline uint16_t zone_card_meta_write_task(uint8_t *buf,
                                                  uint16_t task_bits,
                                                  uint8_t  model_hint,
                                                  uint8_t  priority) {
    ZoneCardMetaHeader h = { META_VERSION_1, META_TASK,
                             (uint16_t)sizeof(ZoneCardMetaTask) };
    ZoneCardMetaTask   t = { task_bits, model_hint, priority };
    memcpy(buf,     &h, sizeof(h));
    memcpy(buf + 4, &t, sizeof(t));
    return (uint16_t)(sizeof(h) + sizeof(t));
}

/* ── Deserialise META_TASK. Returns 1 ok, 0 mismatch/short. ── */
static inline int zone_card_meta_read_task(const uint8_t *buf, uint16_t len,
                                            ZoneCardMetaTask *out) {
    if (!buf || !out || len < 8) return 0;
    const ZoneCardMetaHeader *h = (const ZoneCardMetaHeader *)buf;
    if (h->type_id != META_TASK) return 0;
    memcpy(out, buf + sizeof(ZoneCardMetaHeader), sizeof(ZoneCardMetaTask));
    return 1;
}

/* ── Write META_CUSTOM (arbitrary bytes). buf >= 4 + data_len. ── */
static inline uint16_t zone_card_meta_write_custom(uint8_t *buf,
                                                    const uint8_t *data,
                                                    uint16_t data_len) {
    ZoneCardMetaHeader h = { META_VERSION_1, META_CUSTOM, data_len };
    memcpy(buf, &h, sizeof(h));
    if (data && data_len) memcpy(buf + sizeof(h), data, data_len);
    return (uint16_t)(sizeof(h) + data_len);
}


static inline uint64_t zone_card_hash(const uint8_t *data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x100000001B3ULL; }
    return h;
}

/* ── Entropy ──────────────────────────────────────────────── */
static inline uint8_t zone_card_entropy(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0;
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < len; i++) counts[data[i]]++;
    double ent = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i]) {
            double p = (double)counts[i] / (double)len;
            ent -= p * __builtin_log2(p);
        }
    }
    uint8_t s = (uint8_t)((ent / 8.0) * 255.0);
    return s;
}

/* ── Stability ────────────────────────────────────────────── */
static inline uint8_t zone_card_stability(const float *arr, size_t rows, size_t cols) {
    if (!arr || rows < 2) return 128;
    double sum = 0.0, sum_abs = 0.0;
    for (size_t r = 1; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            double d = (double)arr[r*cols+c] - (double)arr[(r-1)*cols+c];
            sum += (d < 0 ? -d : d);
            double v = (double)arr[(r-1)*cols+c];
            sum_abs += (v < 0 ? -v : v);
        }
    }
    double avg_diff = sum / ((rows-1) * cols);
    double avg_mag  = sum_abs / (rows * cols);
    if (avg_mag < 1e-10) return 255;
    double norm = avg_diff / avg_mag;
    if (norm > 1.0) norm = 1.0;
    return (uint8_t)((1.0 - norm) * 255.0);
}

/* ── Locality from cards ──────────────────────────────────── */
static inline uint8_t zone_card_locality_from_cards(const ZoneCard *self,
                                                     const ZoneCard *neighbors,
                                                     int n_neighbors) {
    if (!neighbors || n_neighbors == 0) return 128;
    int total = 0;
    for (int i = 0; i < n_neighbors; i++) {
        int s = 0;
        if (self->card_type == neighbors[i].card_type) s += 102;
        int ed = (int)self->entropy - (int)neighbors[i].entropy;
        if (ed < 0) ed = -ed;
        s += (76 - ed / 3);
        if (s < 0) s = 0;
        total += s;
    }
    int avg = total / n_neighbors;
    return avg > 255 ? 255 : (uint8_t)(avg < 0 ? 0 : avg);
}

/* ── Sparsity ─────────────────────────────────────────────── */
static inline uint8_t zone_card_sparsity_f(const float *arr, size_t n, float threshold) {
    if (!arr || n == 0) return 0;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; i++) { sum += arr[i]; sum2 += arr[i]*arr[i]; }
    double mean = sum / n, var = sum2 / n - mean*mean;
    if (var < 1e-16) return (mean == 0.0) ? 255 : 0;
    double std = __builtin_sqrt(var);
    size_t near_zero = 0;
    for (size_t i = 0; i < n; i++)
        if (__builtin_fabs((double)arr[i]) < threshold * std) near_zero++;
    return (uint8_t)((near_zero * 255) / n);
}

/* ── Pattern fingerprint ──────────────────────────────────── */
static inline uint16_t zone_card_pattern_f(const float *arr, size_t n) {
    if (!arr || n == 0) return 0;
    uint16_t code = 0;
    if (zone_card_sparsity_f(arr, n, 0.01f) > 127) code |= 0x01;
    if (zone_card_entropy((const uint8_t*)arr, n * sizeof(float)) < 100) code |= 0x02;
    size_t alt = 0;
    for (size_t i = 1; i < n; i++)
        if ((arr[i] > 0 && arr[i-1] < 0) || (arr[i] < 0 && arr[i-1] > 0)) alt++;
    if ((float)alt / n > 0.3f) code |= 0x08;
    uint64_t h = zone_card_hash((const uint8_t*)arr, n * sizeof(float));
    code |= (h & 0xFF) << 8;
    return code;
}

/* ── Card type from float array ───────────────────────────── */
static inline int zone_card_type_f(const float *arr, size_t n) {
    if (!arr || n == 0) return CARD_BATCH;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; i++) { sum += arr[i]; sum2 += arr[i]*arr[i]; }
    double mean = sum / n, var = sum2 / n - mean*mean;
    if (var < 1e-16) return (mean == 0.0) ? CARD_SPARSE : CARD_BATCH;
    if (zone_card_sparsity_f(arr, n, 0.05f) > 178) return CARD_SPARSE;
    return CARD_LZ;
}

/* ── Confidence heuristic ─────────────────────────────────── */
/*
 * Combines stability + locality + entropy into a single 0-255 score.
 * Higher = router is more certain this card is correctly classified.
 */
static inline uint8_t zone_card_confidence(const ZoneCardV3 *c) {
    if (!c) return 0;
    /* stability × 0.4 + locality × 0.4 + (255-entropy) × 0.2 */
    uint32_t score = (uint32_t)c->stability  * 40u
                   + (uint32_t)c->locality   * 40u
                   + (uint32_t)(255u - c->entropy) * 20u;
    score /= 100u;
    return score > 255u ? 255u : (uint8_t)score;
}

/* ── Constructors ─────────────────────────────────────────── */

static inline ZoneCard zone_card_make(const float *arr, size_t n,
                                      uint16_t id,
                                      uint16_t nl, uint16_t nr) {
    ZoneCard card;
    card.id            = id;
    card.card_type     = (uint8_t)zone_card_type_f(arr, n);
    card.entropy       = zone_card_entropy((const uint8_t*)arr, n * sizeof(float));
    card.pattern       = zone_card_pattern_f(arr, n);
    card.locality      = 128;
    card.stability     = 128;
    card.neighbor_left  = nl;
    card.neighbor_right = nr;
    return card;
}

static inline ZoneCardExt zone_card_make_ext(const float *arr, size_t n,
                                              uint16_t id,
                                              uint16_t nl, uint16_t nr) {
    ZoneCardExt card;
    card.id            = id;
    card.card_type     = (uint8_t)zone_card_type_f(arr, n);
    card.entropy       = zone_card_entropy((const uint8_t*)arr, n * sizeof(float));
    card.pattern       = zone_card_pattern_f(arr, n);
    card.locality      = 128;
    card.stability     = 128;
    card.neighbor_left  = nl;
    card.neighbor_right = nr;
    card.hash_val      = zone_card_hash((const uint8_t*)arr, n * sizeof(float));
    return card;
}

/*
 * zone_card_make_v3 — full routing-aware constructor
 *
 * arr        : float tensor data
 * n          : number of floats
 * id         : zone index (0-4095)
 * zone_id    : geo address (0-20735, use NO_ZONE if unknown)
 * nl / nr    : neighbor zone ids
 * parent_id  : parent card in session chain (NO_PARENT = root)
 * ttl        : ticks until stale (0 = never expires)
 * flags      : CARD_FLAG_* bitmask (caller sets HOT_PATH / GLOBE_B etc.)
 */
static inline ZoneCardV3 zone_card_make_v3(const float *arr, size_t n,
                                            uint16_t id,
                                            uint16_t zone_id,
                                            uint16_t nl, uint16_t nr,
                                            uint16_t parent_id,
                                            uint16_t ttl,
                                            uint8_t  flags) {
    ZoneCardV3 card;
    memset(&card, 0, sizeof(card));
    card.id             = id;
    card.card_type      = (uint8_t)zone_card_type_f(arr, n);
    card.entropy        = zone_card_entropy((const uint8_t*)arr, n * sizeof(float));
    card.pattern        = zone_card_pattern_f(arr, n);
    card.locality       = 128;  /* caller updates via zone_card_locality_from_cards */
    card.stability      = (arr && n > 0)
                            ? zone_card_stability(arr, 1, n)
                            : 128;
    card.neighbor_left  = nl;
    card.neighbor_right = nr;
    card.zone_id        = zone_id;
    card.flags          = flags;
    card.parent_id      = parent_id;
    card.ttl            = ttl;
    card.ext_ptr        = 0;  /* caller sets via zone_card_set_ext() */
    card.hash_val       = zone_card_hash((const uint8_t*)arr, n * sizeof(float));
    card.confidence     = zone_card_confidence(&card);
    return card;
}

/* ── TTL helpers ──────────────────────────────────────────── */
static inline int zone_card_is_expired(const ZoneCardV3 *c, uint16_t current_tick) {
    if (!c || c->ttl == 0) return 0;  /* 0 = never expires */
    return (uint16_t)(current_tick - c->id) >= c->ttl;
}

static inline void zone_card_expire(ZoneCardV3 *c) {
    if (c) c->flags |= CARD_FLAG_EXPIRED;
}

/* ── Serialise / deserialise v3 ───────────────────────────── */

/*
 * Writes 29 bytes: [version=0x03][ZoneCardV3 raw]
 * buf must be >= CARD_V3_STREAM_SZ (29) bytes.
 */
static inline void zone_card_v3_write(const ZoneCardV3 *c, uint8_t *buf) {
    buf[0] = CARD_VERSION_3;
    memcpy(buf + 1, c, sizeof(ZoneCardV3));
}

/*
 * Reads from a stream — version-aware.
 * Returns CARD_VERSION_* of what was read, or 0 on error.
 * Always populates dst as ZoneCardV3 (v1/v2 fields zero-extended).
 */
static inline int zone_card_read(const uint8_t *buf, size_t len, ZoneCardV3 *dst) {
    if (!buf || !dst || len < 1) return 0;
    memset(dst, 0, sizeof(*dst));
    dst->parent_id = NO_PARENT;
    dst->zone_id   = NO_ZONE;
    dst->ttl       = 0;

    uint8_t ver = buf[0];

    if (ver == CARD_VERSION_3 && len >= CARD_V3_STREAM_SZ) {
        memcpy(dst, buf + 1, sizeof(ZoneCardV3));
        return CARD_VERSION_3;
    }
    /* v2 extended (20B, no version byte) */
    if (ver != CARD_VERSION_1 && ver != CARD_VERSION_2 && len >= 20) {
        const uint8_t *p = buf;  /* no version byte in v2 stream */
        dst->id             = (uint16_t)(p[0] | (p[1]<<8));
        dst->card_type      = p[2];
        dst->entropy        = p[3];
        dst->pattern        = (uint16_t)(p[4] | (p[5]<<8));
        dst->locality       = p[6];
        dst->stability      = p[7];
        dst->neighbor_left  = (uint16_t)(p[8]  | (p[9]<<8));
        dst->neighbor_right = (uint16_t)(p[10] | (p[11]<<8));
        uint64_t hv = 0;
        for (int i = 0; i < 8; i++) hv |= ((uint64_t)p[12+i]) << (i*8);
        dst->hash_val = hv;
        dst->confidence = zone_card_confidence(dst);
        return CARD_VERSION_2;
    }
    /* v2 packed (12B) */
    if (ver != CARD_VERSION_1 && ver != CARD_VERSION_2 && len >= 12) {
        const uint8_t *p = buf;
        dst->id             = (uint16_t)(p[0] | (p[1]<<8));
        dst->card_type      = p[2];
        dst->entropy        = p[3];
        dst->pattern        = (uint16_t)(p[4] | (p[5]<<8));
        dst->locality       = p[6];
        dst->stability      = p[7];
        dst->neighbor_left  = (uint16_t)(p[8]  | (p[9]<<8));
        dst->neighbor_right = (uint16_t)(p[10] | (p[11]<<8));
        dst->confidence     = zone_card_confidence(dst);
        return CARD_VERSION_2;
    }
    return 0;
}

/* ── Cloud path (logprobs[]) ──────────────────────────────────────────────
 *
 * Input: log-probability array (e.g. from LLM top-k logprobs).
 * Converts via exp() → probability distribution, then derives the same
 * fields as zone_card_make() so both paths produce comparable ZoneCards.
 *
 * entropy  : Shannon H over prob distribution, mapped to [0,255]
 * stability: inverse of max-prob dominance — certain → high, uniform → low
 * card_type: CARD_SPARSE if p_max > 0.5, CARD_LZ if p_max < 0.2, else CARD_BATCH
 * pattern  : deterministic bit fingerprint from prob ranks
 * ──────────────────────────────────────────────────────────────────────── */

#include <math.h>

/* Internal: compute prob[] from logprob[], return p_max */
static inline float _lp_to_prob(const float *lp, size_t n, float *prob) {
    float p_max = 0.0f;
    for (size_t i = 0; i < n; i++) {
        prob[i] = expf(lp[i]);
        if (prob[i] > p_max) p_max = prob[i];
    }
    return p_max;
}

static inline uint8_t _lp_entropy(const float *prob, size_t n) {
    float h = 0.0f;
    for (size_t i = 0; i < n; i++) {
        if (prob[i] > 1e-9f) h -= prob[i] * logf(prob[i]);
    }
    /* normalize: max H = log(n), map to [0,255] */
    float h_max = (n > 1) ? logf((float)n) : 1.0f;
    float norm = h / h_max;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return (uint8_t)(norm * 255.0f);
}

static inline uint8_t _lp_stability(float p_max) {
    /* certain (p_max→1) → stability→255, uniform (p_max→1/n) → stability→0 */
    float s = p_max;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return (uint8_t)(s * 255.0f);
}

static inline int _lp_card_type(float p_max) {
    if (p_max > 0.5f) return CARD_SPARSE;
    if (p_max < 0.2f) return CARD_LZ;
    return CARD_BATCH;
}

static inline uint16_t _lp_pattern(const float *prob, size_t n) {
    /* rank-based fingerprint: top-4 token indices packed into 4×4 bits */
    uint16_t pat = 0;
    for (int rank = 0; rank < 4 && rank < (int)n; rank++) {
        size_t best = 0;
        float  best_p = -1.0f;
        for (size_t i = 0; i < n; i++) {
            if (prob[i] > best_p) { best_p = prob[i]; best = i; }
        }
        pat |= (uint16_t)((best & 0xF) << (rank * 4));
        ((float*)prob)[best] = -1.0f;   /* mark used — safe: local copy */
    }
    return pat;
}

static inline ZoneCard zone_card_from_logprobs(const float *lp, size_t n,
                                               uint16_t id,
                                               uint16_t nl, uint16_t nr) {
    /* stack-allocate prob buffer (n expected small: top-k ≤ 200) */
    float prob[256];
    if (n > 256) n = 256;
    float p_max = _lp_to_prob(lp, n, prob);

    ZoneCard card;
    card.id             = id;
    card.card_type      = (uint8_t)_lp_card_type(p_max);
    card.entropy        = _lp_entropy(prob, n);
    card.pattern        = _lp_pattern(prob, n);   /* prob[] consumed here */
    card.locality       = 128;                    /* unknown without spatial ctx */
    card.stability      = _lp_stability(p_max);
    card.neighbor_left  = nl;
    card.neighbor_right = nr;
    return card;
}

static inline ZoneCardExt zone_card_from_logprobs_ext(const float *lp, size_t n,
                                                      uint16_t id,
                                                      uint16_t nl, uint16_t nr) {
    ZoneCard base = zone_card_from_logprobs(lp, n, id, nl, nr);
    ZoneCardExt card;
    card.id             = base.id;
    card.card_type      = base.card_type;
    card.entropy        = base.entropy;
    card.pattern        = base.pattern;
    card.locality       = base.locality;
    card.stability      = base.stability;
    card.neighbor_left  = base.neighbor_left;
    card.neighbor_right = base.neighbor_right;
    card.hash_val       = zone_card_hash((const uint8_t*)lp, n * sizeof(float));
    return card;
}

/* ── Cross-path helpers ───────────────────────────────────────────────── */

/* Two cards are in the same shell if entropy bands match (32-unit bands) */
static inline int zone_card_same_shell(const ZoneCard *a, const ZoneCard *b) {
    return (a->entropy >> 5) == (b->entropy >> 5);
}

/* Local weight path and cloud logprob path are consistent if:
 *   - card_type agrees, OR
 *   - both have entropy in the same half (low/high) */
static inline int zone_card_src_consistent(const ZoneCard *local,
                                           const ZoneCard *cloud) {
    if (local->card_type == cloud->card_type) return 1;
    return (local->entropy < 128) == (cloud->entropy < 128);
}

/* Bitmask diff: bit0=card_type, bit1=entropy_band, bit2=stability_band,
 *               bit3=pattern_hi, bit4=locality_band */
static inline uint8_t zone_card_diff(const ZoneCard *a, const ZoneCard *b) {
    uint8_t d = 0;
    if (a->card_type          != b->card_type)          d |= 0x01;
    if ((a->entropy  >> 5)    != (b->entropy  >> 5))    d |= 0x02;
    if ((a->stability >> 5)   != (b->stability >> 5))   d |= 0x04;
    if ((a->pattern >> 8)     != (b->pattern >> 8))     d |= 0x08;
    if ((a->locality >> 5)    != (b->locality >> 5))    d |= 0x10;
    return d;
}
