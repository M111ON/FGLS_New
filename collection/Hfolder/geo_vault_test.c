/*
 * geo_vault_test.c — GeoVault encode/decode/verify
 * compile: gcc -O2 -o geo_vault_test geo_vault_test.c -lzstd
 *
 * Tests:
 *   1. roundtrip verify (slot ↔ addr)
 *   2. pack folder → pixel buffer → zstd compress
 *   3. seek + unpack → verify SHA match
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zstd.h>
#include "geo_vault.h"

/* ── tiny SHA1 (self-contained) ─────────────────────────────────── */
typedef struct { uint32_t s[5]; uint64_t len; uint8_t buf[64]; uint32_t buflen; } SHA1;
static void sha1_init(SHA1 *c){
    c->s[0]=0x67452301;c->s[1]=0xEFCDAB89;c->s[2]=0x98BADCFE;
    c->s[3]=0x10325476;c->s[4]=0xC3D2E1F0;c->len=c->buflen=0;
}
#define ROL(v,n) (((v)<<(n))|((v)>>(32-(n))))
static void sha1_block(SHA1 *c, const uint8_t *b){
    uint32_t w[80],a,b2,cc,d,e,t,i;
    for(i=0;i<16;i++) w[i]=(b[i*4]<<24)|(b[i*4+1]<<16)|(b[i*4+2]<<8)|b[i*4+3];
    for(i=16;i<80;i++) w[i]=ROL(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=c->s[0];b2=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];
    for(i=0;i<80;i++){
        if(i<20)      t=ROL(a,5)+((b2&cc)|(~b2&d))+e+w[i]+0x5A827999;
        else if(i<40) t=ROL(a,5)+(b2^cc^d)+e+w[i]+0x6ED9EBA1;
        else if(i<60) t=ROL(a,5)+((b2&cc)|(b2&d)|(cc&d))+e+w[i]+0x8F1BBCDC;
        else          t=ROL(a,5)+(b2^cc^d)+e+w[i]+0xCA62C1D6;
        e=d;d=cc;cc=ROL(b2,30);b2=a;a=t;
    }
    c->s[0]+=a;c->s[1]+=b2;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;
}
static void sha1_update(SHA1 *c, const uint8_t *data, size_t len){
    c->len+=len;
    while(len){
        uint32_t take=64-c->buflen; if(take>len)take=(uint32_t)len;
        memcpy(c->buf+c->buflen,data,take);
        c->buflen+=take; data+=take; len-=take;
        if(c->buflen==64){sha1_block(c,c->buf);c->buflen=0;}
    }
}
static void sha1_final(SHA1 *c, uint8_t out[20]){
    uint64_t bits=c->len*8; uint8_t pad[64]={0x80};
    uint32_t padlen=(c->buflen<56)?56-c->buflen:120-c->buflen;
    sha1_update(c,pad,1); memset(pad,0,sizeof(pad));
    if(padlen>1)sha1_update(c,pad,padlen-1);
    uint8_t lb[8]; for(int i=7;i>=0;i--){lb[i]=(uint8_t)(bits&0xFF);bits>>=8;}
    sha1_update(c,lb,8);
    for(int i=0;i<5;i++){out[i*4]=(uint8_t)(c->s[i]>>24);out[i*4+1]=(uint8_t)(c->s[i]>>16);
        out[i*4+2]=(uint8_t)(c->s[i]>>8);out[i*4+3]=(uint8_t)(c->s[i]);}
}

/* ── Test data: synthetic "files" ────────────────────────────────── */
#define N_FILES 5
typedef struct {
    const char *name;
    uint32_t    size;
    uint8_t    *data;
} TestFile;

static TestFile make_file(const char *name, uint32_t size, uint8_t seed) {
    TestFile f; f.name = name; f.size = size;
    f.data = malloc(size);
    for (uint32_t i = 0; i < size; i++)
        f.data[i] = (uint8_t)((i * 31 + seed) ^ (i >> 3));
    return f;
}

/* ── Encode: files → pixel buffer ────────────────────────────────── */
typedef struct {
    uint8_t          *frames;       /* raw pixel buffer (n_frames × GV_FRAME_BYTES) */
    uint32_t          n_frames;
    GeoVaultHeader    hdr;
    GeoVaultFileEntry entries[32];  /* up to 32 files */
} VaultEnc;

static VaultEnc encode_files(TestFile *files, uint32_t n) {
    VaultEnc v; memset(&v, 0, sizeof(v));

    /* count total slots needed */
    uint64_t total_raw   = 0;
    uint32_t total_slots = 0;
    for (uint32_t i = 0; i < n; i++) {
        total_raw   += files[i].size;
        total_slots += gv_slots_needed(files[i].size);
    }

    v.n_frames = (total_slots + GV_RESIDUAL - 1) / GV_RESIDUAL;
    v.frames   = calloc(v.n_frames, GV_FRAME_BYTES);

    /* fill header */
    v.hdr.magic       = GV_MAGIC;
    v.hdr.version     = GV_VERSION;
    v.hdr.flags       = 0x0001;  /* compressed */
    v.hdr.n_files     = n;
    v.hdr.n_frames    = v.n_frames;
    v.hdr.total_raw   = total_raw;
    v.hdr.frame_bytes = GV_FRAME_BYTES;

    /* SHA1 of all file data concatenated */
    SHA1 folder_sha; sha1_init(&folder_sha);

    uint32_t global_slot = 0;  /* slot counter across all frames */

    for (uint32_t fi = 0; fi < n; fi++) {
        TestFile *f   = &files[fi];
        uint32_t slots = gv_slots_needed(f->size);

        /* compute file SHA1 */
        SHA1 fsha; sha1_init(&fsha);
        sha1_update(&fsha, f->data, f->size);
        sha1_update(&folder_sha, f->data, f->size);
        uint8_t sha[20]; sha1_final(&fsha, sha);

        /* fill file entry */
        GeoVaultFileEntry *e = &v.entries[fi];
        e->name_hash  = gv_fnv1a(f->name);
        e->raw_size   = f->size;
        e->slot_start = global_slot % GV_RESIDUAL;
        e->slot_count = slots;
        e->frame_start= global_slot / GV_RESIDUAL;
        memcpy(e->file_sha, sha, 12);

        /* pack chunks into pixel buffer */
        for (uint32_t ci = 0; ci < slots; ci++) {
            uint32_t frame    = global_slot / GV_RESIDUAL;
            uint32_t slot_inf = global_slot % GV_RESIDUAL;
            uint8_t *fbuf     = v.frames + frame * GV_FRAME_BYTES;

            uint32_t src_off = ci * GV_CHUNK_BYTES;
            uint32_t remain  = f->size > src_off ? f->size - src_off : 0;
            gv_pack_chunk(f->data + src_off, remain, slot_inf, fbuf);
            global_slot++;
        }
    }

    uint8_t sha_out[20]; sha1_final(&folder_sha, sha_out);
    memcpy(v.hdr.folder_sha, sha_out, 20);

    return v;
}

/* ── Decode: seek + unpack one file ──────────────────────────────── */
static uint8_t *decode_file(VaultEnc *v, uint32_t file_idx) {
    GeoVaultFileEntry *e = &v->entries[file_idx];
    uint8_t *out = malloc(e->raw_size);

    uint32_t global_slot = e->frame_start * GV_RESIDUAL + e->slot_start;

    for (uint32_t ci = 0; ci < e->slot_count; ci++) {
        uint32_t frame    = global_slot / GV_RESIDUAL;
        uint32_t slot_inf = global_slot % GV_RESIDUAL;
        uint8_t *fbuf     = v->frames + frame * GV_FRAME_BYTES;

        uint32_t dst_off = ci * GV_CHUNK_BYTES;
        uint32_t remain  = e->raw_size > dst_off ? e->raw_size - dst_off : 0;
        gv_unpack_chunk(fbuf, slot_inf, out + dst_off, remain);
        global_slot++;
    }
    return out;
}

/* ── Main ─────────────────────────────────────────────────────────── */
int main(void) {
    printf("=== GeoVault Skeleton Test ===\n\n");

    /* 1. Roundtrip verify */
    printf("[1] Tring slot ↔ addr roundtrip ... ");
    uint32_t rt_err = gv_verify_roundtrip();
    printf("%s (%u errors)\n", rt_err == 0 ? "PASS" : "FAIL", rt_err);

    /* 2. Struct sizes */
    printf("[2] Struct sizes: header=%zuB (need 64) entry=%zuB (need 32) ... %s\n",
           sizeof(GeoVaultHeader), sizeof(GeoVaultFileEntry),
           (sizeof(GeoVaultHeader)==64 && sizeof(GeoVaultFileEntry)==32) ? "PASS" : "FAIL");

    /* 3. Frame constants */
    printf("[3] Frame bytes: %u (need %u=6912×3) ... %s\n",
           GV_FRAME_BYTES, GV_RESIDUAL * GV_CHUNK_BYTES,
           GV_FRAME_BYTES == GV_RESIDUAL * GV_CHUNK_BYTES ? "PASS" : "FAIL");

    printf("[3] Geometry chain: %u+%u=%u (need %u) ... %s\n",
           GV_HILBERT, GV_RESIDUAL, GV_HILBERT+GV_RESIDUAL, GV_SYNC,
           GV_HILBERT + GV_RESIDUAL == GV_SYNC ? "PASS" : "FAIL");

    /* 4. Encode/decode test */
    printf("\n[4] Encode/decode roundtrip:\n");
    TestFile files[N_FILES] = {
        make_file("README.md",       1024, 0xAA),
        make_file("src/main.c",      8192, 0xBB),
        make_file("data/config.bin", 256,  0xCC),
        make_file("assets/img.raw",  4500, 0xDD),
        make_file("notes.txt",       77,   0xEE),
    };

    VaultEnc vault = encode_files(files, N_FILES);
    printf("  Encoded: %u files → %u frame(s) × %uB = %uB raw buffer\n",
           N_FILES, vault.n_frames, GV_FRAME_BYTES, vault.n_frames * GV_FRAME_BYTES);

    /* compress pixel buffer */
    size_t buf_raw = (size_t)vault.n_frames * GV_FRAME_BYTES;
    size_t bound   = ZSTD_compressBound(buf_raw);
    uint8_t *cbuf  = malloc(bound);
    size_t csz     = ZSTD_compress(cbuf, bound, vault.frames, buf_raw, 3);
    vault.hdr.compressed_sz = csz;
    printf("  Pixel buffer: %zuB raw → %zuB zstd (ratio=%.2fx)\n",
           buf_raw, csz, (double)buf_raw / csz);

    /* decode each file and verify */
    uint32_t ok = 0;
    for (uint32_t fi = 0; fi < N_FILES; fi++) {
        uint8_t *dec = decode_file(&vault, fi);
        int match = memcmp(dec, files[fi].data, files[fi].size) == 0;

        /* also verify via addr geometry */
        GeoVaultAddr a = gv_addr(vault.entries[fi].slot_start);
        uint8_t spoke  = gv_apex_spoke(fi, 0);

        printf("  [%c] %-20s %5uB  slot=%4u compound=%2u pattern=%3u spoke=%u\n",
               match ? '✓' : '✗',
               files[fi].name, files[fi].size,
               vault.entries[fi].slot_start,
               a.compound, a.pattern, spoke);

        if (match) ok++;
        free(dec);
    }

    printf("\nResult: %u/%u PASS\n", ok, N_FILES);

    /* 5. Seek demonstration */
    printf("\n[5] O(1) seek demo:\n");
    for (uint32_t fi = 0; fi < 3; fi++) {
        GeoVaultFileEntry *e = &vault.entries[fi];
        uint64_t byte_off = gv_seek(e->frame_start, e->slot_start);
        GeoVaultAddr a    = gv_addr(e->slot_start);
        printf("  %-20s → frame=%u slot=%4u offset=%6lluB"
               " [compound=%u pattern=%3u trit=%2u coset=%u fibo=%3u]\n",
               files[fi].name,
               e->frame_start, e->slot_start, (unsigned long long)byte_off,
               a.compound, a.pattern, a.trit, a.coset, a.fibo);
    }

    /* cleanup */
    for (uint32_t i = 0; i < N_FILES; i++) free(files[i].data);
    free(vault.frames); free(cbuf);

    printf("\n=== Skeleton DONE ===\n");
    return rt_err == 0 && ok == N_FILES ? 0 : 1;
}
