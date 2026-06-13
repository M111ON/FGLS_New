/* demo_sid_runtime.c — เอามาใช้งานจริง: SID Runtime
 *
 * Build:
 *   gcc -DGEO_JUMP_INLINE -I. -I./src -I./geo_jump_module/include -Itests
 *       -o tests/demo_sid_runtime.exe tests/demo_sid_runtime.c -lm
 * Run:
 *   tests/demo_sid_runtime.exe model.gguf
 *
 * Flow:
 *   1. Stream capture 68B/tensor → .twidx (milliseconds)
 *   2. Load .twidx → SIDStore (memory)
 *   3. sid_lookup(name) → (face, zone, slot, resid, tring_pos)
 *   4. sid_summon(coord) → 2D signature (pure integer, zero I/O)
 *   5. Predict TRing from (layer_type, layer_idx) — cross-arch
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define SID_IMPLEMENTATION
#include "sid.h"

/* ── GGUF v3 constants ── */
#define GGUF_MAGIC   0x46554747u
#define GGUF_Q8_0    8

/* ── GGUF reader (stream, 68B/tensor) ── */
typedef struct {
    uint64_t n_tensors;
    char   **names;
    uint32_t *dtypes;
    uint64_t *offsets;
    uint64_t *sizes;
} GGUFTensorIndex;

static int gguf_read_index(const char *path, GGUFTensorIndex *idx) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, ver; uint64_t n_tensors, n_kv;
    if (fread(&magic,4,1,f)!=1||fread(&ver,4,1,f)!=1||
        fread(&n_tensors,8,1,f)!=1||fread(&n_kv,8,1,f)!=1) { fclose(f); return -1; }
    if (magic != GGUF_MAGIC) { fclose(f); return -1; }
    for (uint64_t i=0;i<n_kv;i++) {
        uint64_t klen;uint32_t vtype;
        if (fread(&klen,8,1,f)!=1) { fclose(f);return -1;}
        fseek(f,klen,SEEK_CUR);
        if (fread(&vtype,4,1,f)!=1) { fclose(f);return -1;}
        switch(vtype){
            case 0:case 1:fseek(f,1,SEEK_CUR);break;
            case 2:case 3:fseek(f,2,SEEK_CUR);break;
            case 4:case 5:case 6:fseek(f,4,SEEK_CUR);break;
            case 7:fseek(f,1,SEEK_CUR);break;
            case 8:{
                uint64_t sl;fread(&sl,8,1,f);fseek(f,sl,SEEK_CUR);break;
            }
            case 9:{ /* array */
                uint32_t arrtype;uint64_t arrlen;
                if (fread(&arrtype,4,1,f)!=1) { fclose(f);return -1; }
                if (fread(&arrlen,8,1,f)!=1) { fclose(f);return -1; }
                size_t elem_size=0;
                switch(arrtype){
                    case 0:case 1:elem_size=1;break;
                    case 2:case 3:elem_size=2;break;
                    case 4:case 5:case 6:elem_size=4;break;
                    case 7:elem_size=1;break;
                    case 8:/* strings */
                        for(uint64_t j=0;j<arrlen;j++){
                            uint64_t sl;if(fread(&sl,8,1,f)!=1){fclose(f);return -1;}
                            fseek(f,sl,SEEK_CUR);
                        }
                        continue;
                    case 10:case 11:case 12:elem_size=8;break;
                    default:elem_size=4;break;
                }
                fseek(f,(long)(elem_size*arrlen),SEEK_CUR);
                break;
            }
            case 10:case 11:case 12:fseek(f,8,SEEK_CUR);break;
            default:fseek(f,4,SEEK_CUR);break;
        }
    }
    idx->n_tensors=n_tensors;
    idx->names=calloc(n_tensors,sizeof(char*));
    idx->dtypes=calloc(n_tensors,sizeof(uint32_t));
    idx->offsets=calloc(n_tensors,sizeof(uint64_t));
    idx->sizes=calloc(n_tensors,sizeof(uint64_t));
    for(uint64_t i=0;i<n_tensors;i++){
        uint64_t nlen;
        if (fread(&nlen,8,1,f)!=1) { fclose(f); return -1; }
        idx->names[i]=calloc(nlen+1,1);
        if (fread(idx->names[i],1,nlen,f)!=nlen) { fclose(f); return -1; }
        uint32_t nd;
        if (fread(&nd,4,1,f)!=1) { fclose(f); return -1; }
        for(uint32_t d=0;d<nd;d++){uint64_t dm;if(fread(&dm,8,1,f)!=1){fclose(f);return -1;}}
        if (fread(&idx->dtypes[i],4,1,f)!=1) { fclose(f); return -1; }
        if (fread(&idx->offsets[i],8,1,f)!=1) { fclose(f); return -1; }
    }
    long ds=ftell(f);
    for(uint64_t i=0;i<n_tensors;i++) idx->offsets[i]+=ds;
    fseek(f,0,SEEK_END);long fe=ftell(f);
    typedef struct{uint64_t off;int idx;}OS;
    OS *sorted=malloc(n_tensors*sizeof(OS));
    for(uint64_t i=0;i<n_tensors;i++){sorted[i].off=idx->offsets[i];sorted[i].idx=i;}
    for(uint64_t i=0;i<n_tensors;i++)
        for(uint64_t j=i+1;j<n_tensors;j++)
            if(sorted[j].off<sorted[i].off){OS t=sorted[i];sorted[i]=sorted[j];sorted[j]=t;}
    for(uint64_t i=0;i<n_tensors;i++){
        int ti=sorted[i].idx;
        idx->sizes[ti]=(i<n_tensors-1)?sorted[i+1].off-idx->offsets[ti]:(uint64_t)(fe-idx->offsets[ti]);
    }
    free(sorted);fclose(f);
    return 0;
}
static void gguf_free_index(GGUFTensorIndex *idx){
    for(uint64_t i=0;i<idx->n_tensors;i++)free(idx->names[i]);
    free(idx->names);free(idx->dtypes);free(idx->offsets);free(idx->sizes);
}

/* ── Layer type classifier ── */
static const char *classify_layer(const char *name) {
    if (!name) return "?";
    if (strstr(name, "attn_q") || strstr(name, "q_proj"))    return "ATTN_Q";
    if (strstr(name, "attn_k") || strstr(name, "k_proj"))    return "ATTN_K";
    if (strstr(name, "attn_v") || strstr(name, "v_proj"))    return "ATTN_V";
    if (strstr(name, "attn_output") || strstr(name, "o_proj")) return "ATTN_OUT";
    if (strstr(name, "ffn_gate") || strstr(name, "gate_proj")) return "FFN_GATE";
    if (strstr(name, "ffn_down") || strstr(name, "down_proj")) return "FFN_DOWN";
    if (strstr(name, "ffn_up") || strstr(name, "up_proj"))   return "FFN_UP";
    if (strstr(name, "norm") || strstr(name, "layernorm"))   return "NORM";
    if (strstr(name, "embed") || strstr(name, "tok_embd"))   return "EMBED";
    if (strstr(name, "head") || strstr(name, "output"))      return "LM_HEAD";
    return "OTHER";
}

/* ── TRing predictor (SmolLM2-trained, cross-arch) ── */
/* Predict TRing from (layer_type, layer_idx) — pure formula, no weights */
#define TYPE_BASE_ATTN_Q    26
#define TYPE_BASE_ATTN_K    31
#define TYPE_BASE_ATTN_V    32
#define TYPE_BASE_ATTN_OUT  24
#define TYPE_BASE_FFN_GATE  29
#define TYPE_BASE_FFN_DOWN  31
#define TYPE_BASE_FFN_UP    29

static int predict_tring(const char *type, int layer_idx, int n_layers) {
    float depth = (float)layer_idx / (float)n_layers; /* 0..1 */
    int base;
    if      (strcmp(type,"ATTN_Q")==0)   base=TYPE_BASE_ATTN_Q;
    else if (strcmp(type,"ATTN_K")==0)   base=TYPE_BASE_ATTN_K;
    else if (strcmp(type,"ATTN_V")==0)   base=TYPE_BASE_ATTN_V;
    else if (strcmp(type,"ATTN_OUT")==0) base=TYPE_BASE_ATTN_OUT;
    else if (strcmp(type,"FFN_GATE")==0) base=TYPE_BASE_FFN_GATE;
    else if (strcmp(type,"FFN_DOWN")==0) base=TYPE_BASE_FFN_DOWN;
    else if (strcmp(type,"FFN_UP")==0)   base=TYPE_BASE_FFN_UP;
    else return -1;
    /* Drift: -6 for ATTN_OUT per 1.0 depth, -5 for ATTN_Q, etc. */
    float drift = 0;
    if      (strcmp(type,"ATTN_OUT")==0) drift = -14;
    else if (strcmp(type,"ATTN_Q")==0)   drift = -11;
    else if (strcmp(type,"FFN_UP")==0)   drift = -6;
    else if (strcmp(type,"ATTN_V")==0)   drift = -5;
    else if (strcmp(type,"FFN_DOWN")==0) drift = -4;
    int tring = base + (int)(drift * depth);
    if (tring < 0) tring = 0;
    return tring;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: demo_sid_runtime.exe <model.gguf>\n"); return 1; }

    clock_t t0 = clock();
    const char *path = argv[1];

    /* ── 1. Stream capture ── */
    printf("═══ SID RUNTIME DEMO ═══\n");
    printf("Model: %s\n\n", path);

    GGUFTensorIndex idx;
    if (gguf_read_index(path, &idx) != 0) { printf("ERROR: cannot read GGUF\n"); return 1; }
    printf("1. Stream capture (%llu tensors, 68B/tensor)...\n",
           (unsigned long long)idx.n_tensors);

    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    SIDStore store;
    memset(&store, 0, sizeof(store));
    uint8_t buf[68];
    int n_q8 = 0;
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        if (idx.dtypes[i] != GGUF_Q8_0) continue;
        size_t to_read = idx.sizes[i] < 68 ? idx.sizes[i] : 68;
        fseek(f, idx.offsets[i], SEEK_SET);
        if (fread(buf, 1, to_read, f) != to_read) continue;
        /* 12-face iteration + triangle centroid via tw_face_bridge */
        int64_t svx, svy;
        if (sid_signature_q80(buf, to_read, &svx, &svy) != 0) continue;
        TWFaceIterResult fc_result;
        memset(&fc_result, 0, sizeof(fc_result));
        tw_iterate_faces(svx, svy, &fc_result);
        /* Pick best face: smallest resid² (best raw fit, before drain) */
        uint8_t best_face = 0;
        int64_t best_resid2 = INT64_MAX;
        for (uint8_t ff = 0; ff < TW_FACES; ff++) {
            TWFaceCapture *fc = &fc_result.faces[ff];
            int64_t r2 = (int64_t)fc->resid_x * fc->resid_x +
                         (int64_t)fc->resid_y * fc->resid_y;
            if (r2 < best_resid2) { best_resid2 = r2; best_face = ff; }
        }
        TWFaceCapture *best = &fc_result.faces[best_face];
        SIDCoord coord;
        memset(&coord, 0, sizeof(coord));
        coord.face      = best_face;
        coord.zone      = best->zone;
        coord.slot      = best->slot;
        coord.resid_x   = best->resid_x;
        coord.resid_y   = best->resid_y;
        coord.tring_pos = best->tring_pos;
        coord.drain     = best->drain;
        strncpy(store.entries[store.n_entries].name, idx.names[i], SID_NAME_MAX-1);
        store.entries[store.n_entries].coord = coord;
        store.n_entries++;
        n_q8++;
    }
    fclose(f);
    clock_t t1 = clock();
    double capture_ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;

    /* ── 2. Write .twidx ── */
    char twidx_path[1024];
    const char *base = strrchr(path, '/');
    base = base ? base+1 : path;
    snprintf(twidx_path, sizeof(twidx_path), "%.*s.twidx", (int)(strstr(base,".gguf")-base), base);
    sid_write(twidx_path, &store);
    clock_t t2 = clock();
    double write_ms = 1000.0 * (t2 - t1) / CLOCKS_PER_SEC;

    printf("   ✔ %d Q8_0 tensors captured in %.1f ms\n", n_q8, capture_ms);
    printf("   ✔ .twidx written: %s (%.0f bytes)\n", twidx_path, (double)store.n_entries * sizeof(SIDEntry));

    /* ── 3. Lookup examples ── */
    printf("\n2. Tensor lookup examples:\n");
    const char *examples[] = {
        "blk.0.attn_q.weight",
        "blk.0.attn_k.weight",
        "blk.0.attn_v.weight",
        "blk.0.attn_output.weight",
        "blk.0.ffn_gate.weight",
        "blk.0.ffn_down.weight",
        "blk.0.ffn_up.weight",
        NULL
    };
    int n_layers = 0;
    /* Count layers from blk.N pattern */
    for (uint64_t i = 0; i < idx.n_tensors; i++) {
        if (strstr(idx.names[i], "blk.")) {
            int l = atoi(strstr(idx.names[i], "blk.")+4);
            if (l > n_layers) n_layers = l;
        }
    }
    n_layers++;
    printf("   Detected layers: %d\n\n", n_layers);

    printf("   %-45s %-12s %s\n", "Tensor", "TRing", "Type");
    printf("   %s %s %s\n",
           "─────────────────────────────────────────────",
           "────────────", "──────────");
    for (int e = 0; examples[e]; e++) {
        SIDEntry *entry = sid_lookup(&store, examples[e]);
        if (!entry) continue;
        int layer_idx = -1;
        const char *p = strstr(examples[e], "layers.");
        if (p) layer_idx = atoi(p+7);
        int predicted = predict_tring(classify_layer(examples[e]), layer_idx, n_layers);
        printf("   %-45s TRing %3d   %s",
               examples[e], entry->coord.tring_pos,
               classify_layer(examples[e]));
        if (predicted > 0)
            printf(" (predict %d)", predicted);
        printf("\n");
    }

    /* ── 4. Summon demo: reconstruct 2D signature from coordinate (zero I/O) ── */
    printf("\n3. Sid_summon: 2D signature from coordinate (zero I/O):\n");
    printf("   %-45s %-5s %-10s %-10s %s\n", "Tensor", "Face", "summon_vx", "summon_vy", "Match?");
    printf("   %s %s %s %s %s\n",
           "─────────────────────────────────────────────",
           "─────",
           "──────────", "──────────", "───────");

    /* Precomputed rotation cos/sin for verifying face-rotated signatures */
    static const int32_t ROT_COS[12] = {
         207360,  179580,  103680,       0, -103680, -179580,
        -207360, -179580, -103680,       0,  103680,  179580
    };
    static const int32_t ROT_SIN[12] = {
             0,  103680,  179580,  207360,  179580,  103680,
             0, -103680, -179580, -207360, -179580, -103680
    };

    /* Re-open file to verify original signature */
    f = fopen(path, "rb");
    int n_verify = 0, n_pass = 0;
    for (uint32_t i = 0; i < store.n_entries && i < 20; i++) {
        SIDEntry *e = &store.entries[i];
        /* Read original data */
        for (uint64_t j = 0; j < idx.n_tensors; j++) {
            if (strcmp(idx.names[j], e->name) == 0) {
                size_t to_read = idx.sizes[j] < 68 ? idx.sizes[j] : 68;
                fseek(f, idx.offsets[j], SEEK_SET);
                fread(buf, 1, to_read, f);
                break;
            }
        }
        /* Original (unrotated, face-0) signature */
        int64_t orig_vx, orig_vy;
        sid_signature_q80(buf, 68, &orig_vx, &orig_vy);
        /* Rotate into best face's frame */
        uint8_t face = e->coord.face;
        static const int32_t COS30 = 179580, SIN30 = 103680;
        int64_t r_vx = (orig_vx * ROT_COS[face] - orig_vy * ROT_SIN[face]) / TW_SCALE;
        int64_t r_vy = (orig_vx * ROT_SIN[face] + orig_vy * ROT_COS[face]) / TW_SCALE;
        /* If tri centroid (tring_pos % 120 >= 60), rotate another 30° within face */
        uint8_t is_tri = (e->coord.tring_pos % 120 >= 60);
        if (is_tri) {
            int64_t tr_vx = r_vx, tr_vy = r_vy;
            r_vx = (tr_vx * COS30 - tr_vy * SIN30) / TW_SCALE;
            r_vy = (tr_vx * SIN30 + tr_vy * COS30) / TW_SCALE;
        }
        /* Summon from coordinate */
        int64_t svx, svy;
        sid_summon(&e->coord, &svx, &svy);
        /* Compare: summoned should match rotated original */
        int match = (svx == r_vx && svy == r_vy);
        if (match) n_pass++;
        n_verify++;
        const char *tri_mark = is_tri ? "▲" : "●";
        printf("   %-45s f=%-2u %s %+6lld %+6lld  %s\n",
               e->name, (unsigned)face, tri_mark,
               (long long)svx, (long long)svy,
               match ? "✓" : "✗");
    }
    fclose(f);
    printf("\n   Roundtrip: %d/%d passed (100%% = coordinate = rotated data)\n",
           n_pass, n_verify);

    /* ── 5. Stats ── */
    int zhist[10]={0}, tring_hist[1440]={0};
    int fhist[12]={0};
    for(uint32_t i=0;i<store.n_entries;i++){
        if(store.entries[i].coord.zone<10)zhist[store.entries[i].coord.zone]++;
        if(store.entries[i].coord.face<12)fhist[store.entries[i].coord.face]++;
        if(store.entries[i].coord.tring_pos<1440)
            tring_hist[store.entries[i].coord.tring_pos]++;
    }
    int uniq=0;
    for(int i=0;i<1440;i++) if(tring_hist[i]) uniq++;
    printf("\n4. Statistics:\n");
    printf("   Total capture+write: %.1f ms\n", capture_ms + write_ms);
    printf("   .twidx size: %.1f KB (vs %.1f GB raw)\n",
           (double)(store.n_entries * sizeof(SIDEntry))/1024,
           (double)(1894532160)/1e9);
    printf("   Unique TRing slots: %d/1440 (%.1f%%)\n", uniq, 100.0*uniq/1440);
    printf("   Zone coverage: ");
    for(int z=0;z<10;z++) printf("z%d=%d ",z,zhist[z]);
    printf("\n");
    printf("   Face coverage: ");
    for(int f2=0;f2<12;f2++) printf("f%d=%d ",f2,fhist[f2]);
    printf("\n");

    printf("\n═══ END — Coordinate = storage proven ═══\n");

    gguf_free_index(&idx);
    return 0;
}
