/*
 * pipe_service_cli.c — CLI frontend for pipe_service.h
 *
 * Each command is self-contained (opens pipe, does work, closes).
 * Use --pipe PATH for DRamTile persistence across commands.
 * Without --pipe, data lives only during that command.
 *
 * Commands:
 *   pipe-svc open TYPE NAME [--pipe PATH]     Open (test open)
 *   pipe-svc write NAME --file PATH [--pipe PATH]
 *   pipe-svc read NAME [--out PATH] [--pipe PATH]
 *   pipe-svc list [--pipe PATH]
 *   pipe-svc classify NAME
 *   pipe-svc status [--pipe PATH]
 *   pipe-svc checkpoint PATH [--pipe PATH]
 *   pipe-svc daemon --pipe PATH              Keep-alive mode (interactive)
 *   pipe-svc version
 *
 * TYPE: tts | vlm | llm | auto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>
#include <windows.h>

#include "pipe_service.h"

#define VERSION "0.3.0"

/* ── Extract --pipe PATH from argv ──────────────────────────── */
static const char *find_pipe_arg(int argc, char **argv) {
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--pipe") == 0) return argv[i + 1];
    }
    return NULL;
}

/* ── Open pipe context from args ────────────────────────────── */
static PipeContext *open_from_args(PipeServiceType type, const char *name,
                                    const char *backend)
{
    return pipe_service_open(type, name, backend);
}

/* ── Parse type string ──────────────────────────────────────── */
static PipeServiceType parse_type(const char *s) {
    if (!s) return PIPE_SVC_LLM;
    if (strcmp(s, "tts") == 0 || strcmp(s, "TTS") == 0) return PIPE_SVC_TTS;
    if (strcmp(s, "vlm") == 0 || strcmp(s, "VLM") == 0) return PIPE_SVC_VLM;
    if (strcmp(s, "llm") == 0 || strcmp(s, "LLM") == 0) return PIPE_SVC_LLM;
    if (strcmp(s, "auto") == 0 || strcmp(s, "AUTO") == 0) return PIPE_SVC_AUTO;
    return PIPE_SVC_LLM;
}

/* ── Help ───────────────────────────────────────────────────── */
static void print_usage(const char *prog) {
    printf("pipe-svc v%s — Unified TTS/VLM/LLM Service CLI\n\n", VERSION);
    printf("Usage:\n");
    printf("  %s open TYPE NAME [--pipe PATH]                  Open (test open)\n", prog);
    printf("  %s write NAME --file PATH [--pipe PATH]          Write tensor from file\n", prog);
    printf("  %s read NAME [--out PATH] [--pipe PATH]          Read tensor\n", prog);
    printf("  %s list [--pipe PATH]                            List tensors\n", prog);
    printf("  %s classify NAME                                 Detect service type\n", prog);
    printf("  %s status [--pipe PATH]                          Show pipe stats\n", prog);
    printf("  %s checkpoint PATH [--pipe PATH]                 Save snapshot\n", prog);
    printf("  %s restore PATH [--pipe PATH]                    Restore snapshot\n", prog);
    printf("  %s close                                         No-op (self-contained)\n", prog);
    printf("  %s speak TEXT [--out PATH] [--voice V] [--lang L] Run Kokoro TTS\n", prog);
    printf("  %s daemon --pipe PATH                            Interactive keep-alive\n", prog);
    printf("  %s version                                       Show version\n", prog);
    printf("\nTYPE: tts | vlm | llm | auto\n");
    printf("\n--pipe PATH: DRamTile file path (persists data across commands)\n");
    printf("             Without --pipe, data stays in RAM only (lost on exit)\n");
    printf("\nExamples:\n");
    printf("  %s open tts kokoro\n", prog);
    printf("  %s write text_encoder.embedding.weight --file data.bin --pipe my.tts\n", prog);
    printf("  %s read decoder.istft.res_weight --pipe my.tts\n", prog);
    printf("  %s status --pipe my.tts\n", prog);
    printf("  %s daemon --pipe my.tts\n", prog);
}

/* ── Commands ───────────────────────────────────────────────── */

static int cmd_open(int argc, char **argv, const char *backend) {
    if (argc < 3) { fprintf(stderr, "Usage: %s open TYPE NAME [--pipe PATH]\n", argv[0]); return 1; }
    PipeServiceType type = parse_type(argv[2]);
    const char *name = argv[3];

    PipeContext *ctx = open_from_args(type, name, backend);
    if (!ctx) { fprintf(stderr, "Failed to open %s pipe\n", pipe_service_name(type)); return 1; }

    printf("Opened %s pipe: %s", pipe_service_name(type), name ? name : "svc");
    if (backend) printf(" [backend: %s]", backend);
    printf("\n");
    pipe_service_close(ctx);
    return 0;
}

static PipeContext *open_auto(const char *backend) {
    return open_from_args(PIPE_SVC_AUTO, "pipe", backend);
}

/* Save slot table to mmap before close (DRamTile persistence) */
static void close_and_checkpoint(PipeContext *ctx, const char *backend) {
    if (backend) pipe_service_checkpoint(ctx, backend);
    pipe_service_close(ctx);
}

static int cmd_write(int argc, char **argv, const char *backend) {
    if (argc < 3) { fprintf(stderr, "Usage: %s write NAME --file PATH [--pipe PATH]\n", argv[0]); return 1; }
    if (!backend) { fprintf(stderr, "Need --pipe PATH for persistent write (state must survive)\n"); return 1; }

    const char *tensor_name = argv[2];
    const char *file_path = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) { file_path = argv[i + 1]; i++; }
    }
    if (!file_path) { fprintf(stderr, "Specify --file PATH\n"); return 1; }

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    int n = pipe_service_write_file(ctx, tensor_name, file_path);
    close_and_checkpoint(ctx, backend);
    if (n < 0) { fprintf(stderr, "Write failed: %d\n", n); return 1; }
    printf("Wrote %d bytes <- %s\n", n, file_path);
    return 0;
}

static int cmd_read(int argc, char **argv, const char *backend) {
    if (argc < 3) { fprintf(stderr, "Usage: %s read NAME [--out PATH] [--pipe PATH]\n", argv[0]); return 1; }
    if (!backend) { fprintf(stderr, "Need --pipe PATH to read persisted data\n"); return 1; }
    const char *tensor_name = argv[2];
    const char *out_path = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out_path = argv[i + 1]; i++; }
    }

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    uint32_t sz = 0;
    const void *data = pipe_service_read(ctx, tensor_name, &sz);
    if (!data) { fprintf(stderr, "Tensor '%s' not found\n", tensor_name); close_and_checkpoint(ctx, backend); return 1; }

    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        if (!f) { fprintf(stderr, "Cannot write %s\n", out_path); close_and_checkpoint(ctx, backend); return 1; }
        fwrite(data, 1, sz, f);
        fclose(f);
        printf("Read %u bytes -> %s\n", sz, out_path);
    } else {
        printf("(%u bytes) ", sz);
        uint32_t show = sz > 64 ? 64 : sz;
        for (uint32_t i = 0; i < show; i++) printf("%02X", ((const uint8_t *)data)[i]);
        if (sz > 64) printf("...");
        printf("\n");
    }
    close_and_checkpoint(ctx, backend);
    return 0;
}

static int cmd_list(const char *backend) {
    if (!backend) { fprintf(stderr, "Need --pipe PATH\n"); return 1; }

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    uint32_t n = pipe_service_count(ctx);
    PipeStats s;
    pipe_stats(ctx, &s);
    printf("%u tensor(s), %llu/%llu bytes used\n",
           n,
           (unsigned long long)s.capacity_used,
           (unsigned long long)s.capacity_total);

    /* Dump slot names */
    if (n > 0) {
        printf("Tensors:\n");
        /* We can't iterate by name from the public pipe API (linear scan by name) */
        /* For now just show count */
    }

    close_and_checkpoint(ctx, backend);
    return 0;
}

static int cmd_status(const char *backend) {
    if (!backend) { fprintf(stderr, "Need --pipe PATH\n"); return 1; }

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    pipe_service_status(ctx);
    pipe_service_close(ctx);
    return 0;
}

static int cmd_classify(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s classify NAME\n", argv[0]); return 1; }
    const char *name = argv[2];
    PipeServiceType t = pipe_service_classify(name);
    const char *type_str = "unknown";
    switch (t) {
        case PIPE_SVC_TTS: type_str = "tts (Kokoro)"; break;
        case PIPE_SVC_VLM: type_str = "vlm (moondream2)"; break;
        case PIPE_SVC_LLM: type_str = "llm (general)"; break;
        default: break;
    }
    printf("%s -> %s\n", name, type_str);
    return 0;
}

static int cmd_checkpoint(int argc, char **argv, const char *backend) {
    if (argc < 3) { fprintf(stderr, "Usage: %s checkpoint PATH [--pipe BAK]\n", argv[0]); return 1; }
    if (!backend) { fprintf(stderr, "Need --pipe PATH\n"); return 1; }
    const char *snap_path = argv[2];

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    int rc = pipe_service_checkpoint(ctx, snap_path);
    close_and_checkpoint(ctx, backend);
    if (rc != PIPE_OK) { fprintf(stderr, "Checkpoint failed: %d\n", rc); return 1; }
    printf("Checkpoint saved to %s\n", snap_path);
    return 0;
}

static int cmd_restore(int argc, char **argv, const char *backend) {
    if (argc < 3) { fprintf(stderr, "Usage: %s restore PATH [--pipe PATH]\n", argv[0]); return 1; }
    if (!backend) { fprintf(stderr, "Need --pipe PATH\n"); return 1; }
    const char *snap_path = argv[2];

    PipeContext *ctx = open_auto(backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    int rc = pipe_service_restore(ctx, snap_path);
    close_and_checkpoint(ctx, backend);
    if (rc != PIPE_OK) { fprintf(stderr, "Restore failed: %d\n", rc); return 1; }
    printf("Restored from %s\n", snap_path);
    return 0;
}

/* ── close — no-op for single-shot CLI (each invoc is ephemeral) ─ */
static int cmd_close(void) {
    printf("OK (pipe-svc runs self-contained per command)\n");
    return 0;
}

/* ── speak — invoke Kokoro Python TTS (real inference) ────────── */
static int cmd_speak(int argc, char **argv, const char *backend) {
    (void)backend;
    if (argc < 3) { fprintf(stderr, "Usage: %s speak TEXT [--out PATH] [--voice V] [--lang L]\n", argv[0]); return 1; }
    const char *text = argv[2];
    const char *out = "speech.wav";
    const char *voice = "af_heart";
    const char *lang = "a";
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--voice") == 0 && i + 1 < argc) voice = argv[++i];
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) lang = argv[++i];
    }
    /* locate kokoro_tts.py next to this exe */
    char py[1024];
    DWORD sz = GetModuleFileNameA(NULL, py, sizeof(py));
    while (sz > 0 && py[sz-1] != '\\' && py[sz-1] != '/') sz--;
    py[sz] = 0;
    strcat_s(py, sizeof(py), "kokoro_tts.py");
    if (_access(py, 0) != 0) { fprintf(stderr, "kokoro_tts.py not found next to exe\n"); return 1; }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "python \"%s\" --text \"%s\" --out \"%s\" --voice %s --lang %s",
             py, text, out, voice, lang);
    printf("Running Kokoro TTS...\n");
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "TTS failed (rc=%d)\n", rc); return 1; }
    return 0;
}

static int cmd_daemon(const char *backend) {
    if (!backend) { fprintf(stderr, "Usage: pipe-svc daemon --pipe PATH\n"); return 1; }

    PipeContext *ctx = open_from_args(PIPE_SVC_AUTO, "daemon", backend);
    if (!ctx) { fprintf(stderr, "Failed to open pipe\n"); return 1; }

    printf("pipe-svc daemon (pipe: %s, %u slots)\n", backend, pipe_service_count(ctx));
    printf("Commands: status, list, write, read, close\n");
    printf("Type 'quit' to exit.\n\n");

    char line[512];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        if (strcmp(line, "status") == 0) {
            pipe_service_status(ctx);
        } else if (strcmp(line, "list") == 0) {
            uint32_t n = pipe_service_count(ctx);
            PipeStats s;
            pipe_stats(ctx, &s);
            printf("%u tensors(s), %llu/%llu bytes\n",
                   n, (unsigned long long)s.capacity_used,
                   (unsigned long long)s.capacity_total);
        } else if (strncmp(line, "write ", 6) == 0) {
            /* write tensor_name file_path */
            char name[128], path[256];
            if (sscanf(line + 6, "%127s %255s", name, path) == 2) {
                int n = pipe_service_write_file(ctx, name, path);
                if (n < 0) printf("Error: %d\n", n);
                else printf("Wrote %d bytes\n", n);
            } else {
                printf("Usage: write tensor_name file_path\n");
            }
        } else if (strncmp(line, "read ", 5) == 0) {
            char name[128];
            if (sscanf(line + 5, "%127s", name) == 1) {
                uint32_t sz = 0;
                const void *data = pipe_service_read(ctx, name, &sz);
                if (!data) printf("Not found\n");
                else printf("(%u bytes) %02X...%02X\n", sz,
                            ((const uint8_t *)data)[0],
                            ((const uint8_t *)data)[sz-1]);
            } else {
                printf("Usage: read tensor_name\n");
            }
        } else if (strcmp(line, "close") == 0) {
            pipe_service_close(ctx);
            ctx = open_from_args(PIPE_SVC_AUTO, "daemon", backend);
            if (!ctx) { printf("Failed to reopen\n"); break; }
            printf("Reopened\n");
        } else {
            printf("Commands: status, list, write name path, read name, close, quit\n");
        }
    }

    pipe_service_close(ctx);
    printf("Daemon stopped.\n");
    return 0;
}

/* ── Main ───────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 0; }

    const char *cmd = argv[1];
    const char *backend = find_pipe_arg(argc, argv);

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]); return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("pipe-svc v%s\n", VERSION); return 0;
    }
    if (strcmp(cmd, "open") == 0)       return cmd_open(argc, argv, backend);
    if (strcmp(cmd, "write") == 0)      return cmd_write(argc, argv, backend);
    if (strcmp(cmd, "read") == 0)       return cmd_read(argc, argv, backend);
    if (strcmp(cmd, "list") == 0)       return cmd_list(backend);
    if (strcmp(cmd, "status") == 0)     return cmd_status(backend);
    if (strcmp(cmd, "classify") == 0)   return cmd_classify(argc, argv);
    if (strcmp(cmd, "checkpoint") == 0) return cmd_checkpoint(argc, argv, backend);
    if (strcmp(cmd, "restore") == 0)    return cmd_restore(argc, argv, backend);
    if (strcmp(cmd, "daemon") == 0)     return cmd_daemon(backend);
    if (strcmp(cmd, "close") == 0)      return cmd_close();
    if (strcmp(cmd, "speak") == 0)      return cmd_speak(argc, argv, backend);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
