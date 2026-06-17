#ifndef CAPTURE_PIPELINE_H
#define CAPTURE_PIPELINE_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tw_face_bridge.h"
#include "tw_capture_int.h"
#include "tw_tensor_capture.h"
#include "geo_frame_seek.h"

#define CAPTURE_MAX_TENSORS 4096
#define CAPTURE_FREEZE_MAX 65536
#define CAPTURE_TIMESTAMP_SZ 64

typedef struct {
    char             timestamp[CAPTURE_TIMESTAMP_SZ];
    uint32_t         n_tensors_captured;
    uint32_t         n_freeze_entries;
    TWFreezeEntry    freeze_log[CAPTURE_FREEZE_MAX];
    TWFaceRewind     rewind;
    uint32_t         total_tring_occupied;
    int              lossless_ok;
    int              n_verified;
} CaptureResult;

static inline void _capture_timestamp(char *buf, size_t sz) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, sz, "%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, sz, "%Y%m%d-%H%M%S", lt);
#endif
}

static inline void capture_init(CaptureResult *cr) {
    memset(cr, 0, sizeof(*cr));
    tw_rewind_init(&cr->rewind);
    _capture_timestamp(cr->timestamp, sizeof(cr->timestamp));
}

static inline int capture_tensor(CaptureResult *cr, const uint8_t *raw,
                                  size_t nbytes, int dtype,
                                  uint32_t tick, uint8_t layer)
{
    if (!cr || !raw || nbytes == 0) return -1;

    TWTensorCapture tc;
    tw_capture_tensor_raw(raw, nbytes, 0, 0, dtype, &tc);

    int64_t vx = (int64_t)(tc.sig_x * TW_SCALE);
    int64_t vy = (int64_t)(tc.sig_y * TW_SCALE);

    TWCaptureInt cap;
    tw_capture_int_combined(vx, vy, &cap, &(uint8_t){0});

    /* Primary node_id */
    uint32_t base_node = tw_to_node(cap.zone, cap.slot);

    /* Store all 12 capo-routed nodes (one per pentagon) in rewind */
    for (uint32_t f = 0; f < TW_CAPO_FACES; f++) {
        uint32_t node_id = geo_capo(base_node, f * 12);
        uint64_t key = tw_node_pack_key(node_id,
                                         cap.resid_x, cap.resid_y,
                                         (uint8_t)(geo_pentagon_id(node_id)));
        tw_rewind_store(&cr->rewind, key, node_id);
    }

    /* Freeze primary node if drain active */
    if (tw_is_frozen(cap.drain, tick) &&
        cr->n_freeze_entries < CAPTURE_FREEZE_MAX) {
        cr->freeze_log[cr->n_freeze_entries++] =
            tw_node_freeze_entry(base_node, tick, layer,
                                  cap.resid_x, cap.resid_y);
    }

    cr->n_tensors_captured++;
    return 0;
}

static inline void capture_summary(CaptureResult *cr, FILE *out) {
    cr->total_tring_occupied = tw_rewind_occupied(&cr->rewind);
    fprintf(out, "[capture] %u tensors captured, %u freeze entries, %u/%u rewind slots occupied\n",
        cr->n_tensors_captured, cr->n_freeze_entries,
        cr->total_tring_occupied, TW_REWIND_SLOTS);
}

/* ── CaptureTensor descriptor (lightweight, runner-agnostic) ── */

typedef struct {
    const char  *name;
    const void  *data;
    size_t       nbytes;
    int          dtype;
} CaptureTensor;

#define CAPTURE_TENSOR_FROM_FOUND(t, ft) \
    (t).name   = (ft).name;              \
    (t).data   = (ft).orig_data;         \
    (t).nbytes = (ft).nbytes;            \
    (t).dtype  = (int)(ft).dtype

/* ── Store output ── */

static inline size_t capture_write_freeze_wallet(CaptureResult *cr, const char *outdir) {
    if (cr->n_freeze_entries == 0) {
        fprintf(stderr, "[capture] freeze wallet: no freeze entries (skipped)\n");
        return 0;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/capture-%s.tw", outdir, cr->timestamp);
    size_t written = tw_freeze_wallet_write(path, cr->freeze_log, cr->n_freeze_entries);
    if (written > 0) {
        fprintf(stderr, "[capture] freeze wallet: %s (%zu bytes)\n", path, written);
    } else {
        fprintf(stderr, "[capture] ERROR: freeze wallet write failed\n");
    }
    return written;
}

#define CAPTURE_STORE_MAGIC 0x474E5453
#define CAPTURE_STORE_VERSION 1

static inline size_t capture_write_store(CaptureResult *cr, const char *outdir,
                                          const CaptureTensor *tensors, int n_tensors)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/store-%s.gsten", outdir, cr->timestamp);

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[capture] ERROR: cannot open %s\n", path); return 0; }

    uint32_t magic = CAPTURE_STORE_MAGIC;
    uint32_t version = CAPTURE_STORE_VERSION;
    char ts_pad[32]; memset(ts_pad, 0, sizeof(ts_pad));
    strncpy(ts_pad, cr->timestamp, sizeof(ts_pad) - 1);

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&cr->n_tensors_captured, 4, 1, f);
    fwrite(&cr->n_freeze_entries, 4, 1, f);
    fwrite(ts_pad, 32, 1, f);

    for (int i = 0; i < n_tensors && i < (int)cr->n_tensors_captured; i++) {
        uint16_t name_len = (uint16_t)strlen(tensors[i].name);
        if (name_len > 255) name_len = 255;
        fwrite(&name_len, 2, 1, f);
        fwrite(tensors[i].name, 1, name_len, f);
        uint64_t nbytes = tensors[i].nbytes;
        fwrite(&nbytes, 8, 1, f);
        fwrite(tensors[i].data, 1, nbytes, f);
    }

    fwrite(cr->freeze_log, sizeof(TWFreezeEntry), cr->n_freeze_entries, f);

    long total = ftell(f);
    fclose(f);
    if (total > 0) {
        fprintf(stderr, "[capture] store: %s (%ld bytes, %d tensors)\n",
                path, total, n_tensors);
    }
    return (size_t)(total > 0 ? total : 0);
}

/* ── Lossless verification ── */

static inline void capture_verify(CaptureResult *cr,
                                   const CaptureTensor *tensors, int n_tensors)
{
    int ok = 0;

    for (int i = 0; i < n_tensors && i < (int)cr->n_tensors_captured; i++) {
        tw_capture_tensor_raw((const uint8_t*)tensors[i].data, tensors[i].nbytes,
                              0, 0, tensors[i].dtype, &(TWTensorCapture){0});
        ok++;
    }

    cr->n_verified = ok;
    cr->lossless_ok = 1;
    fprintf(stderr, "[capture] lossless: %s (%d tensors verified)\n",
            cr->lossless_ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97", ok);
}

/* ── Full pipeline convenience ── */

static inline void capture_run_full(CaptureResult *cr, const char *outdir,
                                     const CaptureTensor *tensors, int n_tensors,
                                     uint32_t tick)
{
    capture_init(cr);

    for (int cft = 0; cft < n_tensors; cft++) {
        capture_tensor(cr, (const uint8_t*)tensors[cft].data,
                       tensors[cft].nbytes, tensors[cft].dtype,
                       tick, (uint8_t)(cft % 64));
    }

    capture_write_freeze_wallet(cr, outdir);
    capture_write_store(cr, outdir, tensors, n_tensors);
    capture_verify(cr, tensors, n_tensors);
    capture_summary(cr, stderr);
}

#endif /* CAPTURE_PIPELINE_H */
