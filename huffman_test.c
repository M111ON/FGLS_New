#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Simple canonical Huffman over 256 symbols, order-0 ─────────── */

typedef struct { uint32_t freq; int left, right; } HNode;

static int build_huffman(uint32_t *hist, HNode *nodes, int *root_out) {
    int n = 0;
    int active[256], n_active = 0;
    for (int v = 0; v < 256; v++) {
        if (hist[v] == 0) continue;
        nodes[n].freq = hist[v]; nodes[n].left = -1; nodes[n].right = -1;
        active[n_active++] = n;
        n++;
    }
    if (n_active == 1) { /* single symbol: still need 1 node structure */
        nodes[n].freq = nodes[active[0]].freq; nodes[n].left = active[0]; nodes[n].right = -1;
        *root_out = n; n++;
        return n;
    }
    while (n_active > 1) {
        /* find two smallest */
        int i1 = 0, i2 = 1;
        if (nodes[active[i2]].freq < nodes[active[i1]].freq) { int t=i1;i1=i2;i2=t; }
        for (int i = 2; i < n_active; i++) {
            if (nodes[active[i]].freq < nodes[active[i1]].freq) { i2=i1; i1=i; }
            else if (nodes[active[i]].freq < nodes[active[i2]].freq) { i2=i; }
        }
        int a = active[i1], b = active[i2];
        nodes[n].freq = nodes[a].freq + nodes[b].freq;
        nodes[n].left = a; nodes[n].right = b;
        /* remove i1,i2 (larger idx first), add new node */
        int lo = i1<i2?i1:i2, hi=i1<i2?i2:i1;
        active[hi] = active[n_active-1]; n_active--;
        active[lo] = n; /* replace lo slot with new node (n_active unchanged net -1) */
        n++;
    }
    *root_out = active[0];
    return n;
}

static void assign_codes(HNode *nodes, int idx, uint32_t code, int len,
                          uint32_t *out_code, uint8_t *out_len, int sym_base) {
    if (nodes[idx].left == -1 && nodes[idx].right == -1) {
        /* leaf = symbol index (idx itself is symbol slot if idx < sym_base) */
        out_code[idx] = code; out_len[idx] = (len == 0) ? 1 : (uint8_t)len;
        return;
    }
    if (nodes[idx].left != -1) assign_codes(nodes, nodes[idx].left, (code<<1), len+1, out_code, out_len, sym_base);
    if (nodes[idx].right != -1) assign_codes(nodes, nodes[idx].right, (code<<1)|1, len+1, out_code, out_len, sym_base);
}

/* Encode: returns bytes written. code_table/len_table indexed by symbol-node-index (0..n_syms-1 map to values via val_of[]) */
static uint32_t huffman_encode(const int8_t *w, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t hist[256] = {0};
    for (uint32_t i = 0; i < n; i++) hist[(uint8_t)w[i]]++;

    HNode nodes[512];
    int root;
    int total_nodes = build_huffman(hist, nodes, &root);

    /* symbol leaves are nodes[0..n_syms-1] in the order we added them (value order) */
    int val_of[256], n_syms = 0;
    for (int v = 0; v < 256; v++) if (hist[v]) val_of[n_syms++] = v;

    /* lengths come from the Huffman tree */
    uint32_t code_table[512] = {0};
    uint8_t len_table[512] = {0};
    assign_codes(nodes, root, 0, 0, code_table, len_table, n_syms);

    /* Canonical codes from lengths — MUST mirror the decoder's assignment
       exactly so packed bits match what huffman_decode reconstructs. */
    int order[256];
    for (int s = 0; s < n_syms; s++) order[s] = s;
    /* stable insertion sort by len_table[] ascending */
    for (int i = 1; i < n_syms; i++) {
        int key = order[i], j = i - 1;
        while (j >= 0 && len_table[order[j]] > len_table[key]) { order[j+1] = order[j]; j--; }
        order[j+1] = key;
    }
    uint32_t canon = 0; int prev_len = len_table[order[0]];
    for (int i = 0; i < n_syms; i++) {
        int s = order[i];
        if ((int)len_table[s] > prev_len) { canon <<= (len_table[s] - prev_len); prev_len = len_table[s]; }
        code_table[s] = canon;
        canon++;
    }

    /* Header: n_syms(2B) + for each symbol: value(1B)+bitlen(1B) = compact code table */
    uint32_t off = 0;
    out[off++] = (uint8_t)(n_syms & 0xFF);
    out[off++] = (uint8_t)((n_syms >> 8) & 0xFF);
    for (int s = 0; s < n_syms; s++) {
        out[off++] = (uint8_t)val_of[s];
        out[off++] = len_table[s];
    }
    /* bit-pack codes in original order */
    uint32_t bitpos = 0;
    memset(out + off, 0, cap - off);
    uint32_t byte_start = off;
    uint8_t val_to_sym[256];
    for (int s = 0; s < n_syms; s++) val_to_sym[val_of[s]] = (uint8_t)s;

    for (uint32_t i = 0; i < n; i++) {
        int s = val_to_sym[(uint8_t)w[i]];
        uint32_t code = code_table[s];
        uint8_t len = len_table[s];
        for (int b = len - 1; b >= 0; b--) {
            uint32_t bytepos = byte_start + (bitpos >> 3);
            if (bytepos >= cap) return 0;
            if ((code >> b) & 1) out[bytepos] |= (uint8_t)(1 << (7 - (bitpos & 7)));
            bitpos++;
        }
    }
    uint32_t total_bytes = byte_start + (bitpos + 7) / 8;
    (void)total_nodes;
    return total_bytes;
}

/* ── Decoder (rebuilds huffman tree from stored code table) ─────── */
static int decode_cmp(const void *a, const void *b) {
    const uint8_t *la = (const uint8_t*)a, *lb = (const uint8_t*)b;
    return (int)la[1] - (int)lb[1]; /* sort by bitlen for canonical assign - not used, direct decode instead */
}

static int huffman_decode(const uint8_t *in, uint32_t in_len, int8_t *out, uint32_t out_n) {
    (void)decode_cmp;
    uint32_t off = 0;
    if (in_len < 2) return -1;
    int n_syms = in[0] | (in[1] << 8);
    off = 2;
    uint8_t val_of[256], len_of[256];
    for (int s = 0; s < n_syms; s++) {
        val_of[s] = in[off++]; len_of[s] = in[off++];
    }
    /* rebuild codes identical to encoder's canonical Huffman using same greedy algorithm.
       We must reconstruct HNode tree exactly as encoder did to get same codes.
       Simplest robust approach for THIS TEST: re-run build_huffman using same histogram order
       is not available at decode time from lengths alone without canonical-code convention.
       For this test, rebuild by re-deriving from lengths via canonical Huffman codes
       (standard technique): sort symbols by (len, val_of order), assign codes canonically. */
    /* Canonical code assignment from lengths (Huffman canonical form) */
    int order[256];
    for (int i = 0; i < n_syms; i++) order[i] = i;
    /* stable sort by len_of[order[i]] */
    for (int i = 1; i < n_syms; i++) {
        int key = order[i], j = i - 1;
        while (j >= 0 && len_of[order[j]] > len_of[key]) { order[j+1] = order[j]; j--; }
        order[j+1] = key;
    }
    uint32_t code = 0; int prev_len = len_of[order[0]];
    uint32_t code_of[256]; uint8_t len_out[256];
    for (int i = 0; i < n_syms; i++) {
        int s = order[i];
        if (len_of[s] > prev_len) { code <<= (len_of[s] - prev_len); prev_len = len_of[s]; }
        code_of[s] = code; len_out[s] = len_of[s];
        code++;
    }

    /* build a simple binary trie for decode: map (len,code)->symbol via linear scan (fine for <=256 syms) */
    uint32_t bitpos = 0;
    uint32_t byte_start = off;
    for (uint32_t k = 0; k < out_n; k++) {
        uint32_t cur = 0; int len = 0;
        int found = -1;
        while (len < 24) {
            uint32_t bytepos = byte_start + (bitpos >> 3);
            if (bytepos >= in_len) return -2;
            int bit = (in[bytepos] >> (7 - (bitpos & 7))) & 1;
            cur = (cur << 1) | (uint32_t)bit;
            bitpos++; len++;
            for (int s = 0; s < n_syms; s++) {
                if ((int)len_out[s] == len && code_of[s] == cur) { found = s; break; }
            }
            if (found >= 0) break;
        }
        if (found < 0) return -3;
        out[k] = (int8_t)val_of[found];
    }
    return 0;
}

/* ── Test ─────────────────────────────────────────────────────── */
static void gen_gaussian_like(int8_t *w, uint32_t n, unsigned seed) {
    srand(seed);
    for (uint32_t i = 0; i < n; i++) {
        double sum = 0;
        for (int k = 0; k < 6; k++) sum += (double)rand() / RAND_MAX;
        sum = (sum / 6.0 - 0.5) * 2.0;
        int v = (int)(sum * 40);
        if (v > 127) v = 127; if (v < -128) v = -128;
        w[i] = (int8_t)v;
    }
}
static void gen_uniform_random(int8_t *w, uint32_t n, unsigned seed) {
    srand(seed);
    for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);
}
static void gen_sparse(int8_t *w, uint32_t n, unsigned seed) {
    srand(seed);
    for (uint32_t i = 0; i < n; i++)
        w[i] = (rand() % 100 < 70) ? 0 : (int8_t)(rand() % 256 - 128);
}
static void gen_blocky(int8_t *w, uint32_t n, unsigned seed) {
    srand(seed);
    int8_t cur = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 32 == 0) cur = (int8_t)(rand() % 40 - 20);
        w[i] = (int8_t)(cur + (rand() % 5 - 2));
    }
}

static double shannon_entropy(const int8_t *w, uint32_t n) {
    uint32_t hist[256] = {0};
    for (uint32_t i = 0; i < n; i++) hist[(uint8_t)w[i]]++;
    double h = 0.0;
    for (int v = 0; v < 256; v++) {
        if (hist[v] == 0) continue;
        double p = (double)hist[v] / n;
        h -= p * log2(p);
    }
    return h;
}

static void run(const char *name, int8_t *w, uint32_t n) {
    uint32_t cap = n + 8192;
    uint8_t *buf = malloc(cap);
    uint32_t enc = huffman_encode(w, n, buf, cap);
    double h = shannon_entropy(w, n);
    double ideal_bytes = h * n / 8.0;

    /* roundtrip verify */
    int8_t *dec = malloc(n);
    int rc = huffman_decode(buf, enc, dec, n);
    uint32_t mm = 0;
    if (rc == 0) {
        for (uint32_t i = 0; i < n; i++) if (dec[i] != w[i]) mm++;
    } else mm = n;

    printf("%-22s H=%.3f bit/w  huffman=%uB  ideal=%.0fB  overhead=%.1f%%  ratio=%.2fx  lossless=%s(mm=%u)\n",
           name, h, enc, ideal_bytes,
           100.0 * (enc - ideal_bytes) / (ideal_bytes>0?ideal_bytes:1),
           (double)n / enc,
           (rc == 0 && mm == 0) ? "YES" : "NO",
           mm);
    free(buf); free(dec);
}

/* Encode a real Q8_0 tensor's weights (32 per 34B block, skip 2B scale) */
static void run_real(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return; }
    uint32_t magic; fread(&magic,4,1,f);
    if (magic != 0x46554747) { /* "GGUF" */ printf("%s: not GGUF\n", path); fclose(f); return; }
    uint32_t ver; fread(&ver,4,1,f);
    uint64_t nt, nk; fread(&nt,8,1,f); fread(&nk,8,1,f);
    for (uint64_t i=0;i<nk;i++){
        uint64_t kl; fread(&kl,8,1,f); fseek(f,kl,SEEK_CUR);
        uint32_t vt; fread(&vt,4,1,f);
        switch(vt){case 0:case 1:case 7:fseek(f,1,SEEK_CUR);break;case 2:case 3:fseek(f,2,SEEK_CUR);break;
            case 4:case 5:case 6:fseek(f,4,SEEK_CUR);break;
            case 8:{uint64_t l;fread(&l,8,1,f);fseek(f,l,SEEK_CUR);break;}
            case 9:{uint32_t et;fread(&et,4,1,f);uint64_t al;fread(&al,8,1,f);
                    for(uint64_t j=0;j<al;j++){if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}
                    else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);}break;}
            case 10:case 11:case 12:fseek(f,8,SEEK_CUR);break;
            default:fclose(f);return;}
    }
    for (uint64_t i=0;i<nt;i++){
        uint64_t nl; fread(&nl,8,1,f); fseek(f,nl,SEEK_CUR);
        uint32_t nd; fread(&nd,4,1,f); uint64_t nw=1;
        for (uint32_t d=0;d<nd&&d<4;d++){uint64_t dm;fread(&dm,8,1,f);nw*=dm;}
        uint32_t dt; fread(&dt,4,1,f); uint64_t off; fread(&off,8,1,f);
        if (dt == 8) { /* Q8_0 */
            long ds = ftell(f);
            uint32_t nblocks = (uint32_t)(nw/32);
            uint32_t cap_n = nblocks > 4000 ? 4000 : nblocks; /* sample */
            uint8_t *raw = malloc((size_t)cap_n*34);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, (size_t)cap_n*34, f);
            int8_t *w = malloc((size_t)cap_n*32);
            for (uint32_t b=0;b<cap_n;b++)
                for (int j=0;j<32;j++) w[b*32+j] = (int8_t)raw[b*34+2+j];
            uint32_t n = cap_n*32;
            uint32_t cap = n+8192;
            uint8_t *buf = malloc(cap);
            uint32_t enc = huffman_encode(w, n, buf, cap);
            int8_t *dec = malloc(n);
            int rc = huffman_decode(buf, enc, dec, n);
            uint32_t mm=0;
            if (rc==0){ for(uint32_t k=0;k<n;k++) if(dec[k]!=w[k]) mm++; } else mm=n;
            double h = shannon_entropy(w, n);
            double ideal = h*n/8.0;
            printf("REAL[Q8_0:%s] H=%.3f  huffman=%uB  ideal=%.0fB  ratio=%.3fx  lossless=%s(mm=%u)\n",
                   path, h, enc, ideal, (double)n/enc, (rc==0&&mm==0)?"YES":"NO", mm);
            free(raw); free(w); free(buf); free(dec);
            fclose(f); return;
        }
    }
    fclose(f);
    printf("%s: no Q8_0 tensor\n", path);
}

int main(int argc, char **argv) {
    uint32_t n = 100000;
    int8_t *w = malloc(n);

    gen_gaussian_like(w, n, 42); run("gaussian-like", w, n);
    gen_uniform_random(w, n, 100); run("uniform-random", w, n);
    gen_sparse(w, n, 200); run("sparse(70% zero)", w, n);
    gen_blocky(w, n, 300); run("blocky(Q8_0-like)", w, n);
    memset(w, 42, n); run("all-same", w, n);
    for (uint32_t i=0;i<n;i++) w[i]=(i%2)?1:-1; run("alternating(+-1)", w, n);

    free(w);

    if (argc > 1) {
        for (int i = 1; i < argc; i++) run_real(argv[i]);
    } else {
        run_real("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf");
    }
    return 0;
}
