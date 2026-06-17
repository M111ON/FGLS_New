---
phase: 3
slug: runner-integration-demo
status: ready
nyquist_compliant: true
wave_0_complete: false
created: 2026-06-16
---

# Phase 3 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Custom C test (no framework) |
| **Build command** | `cd runner && gcc -O2 -std=c11 -I. -I../collection -I../collection/src -I../collection/core -I../collection/geopixel -II:/llama.cpp/include -II:/llama.cpp/ggml/include -o test_capture.exe tests/test_capture.c llama_pogls_runner_sid_v2.c I:/llama/llama-b9528-bin-win-vulkan-x64/llama.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-base.dll I:/llama/llama-b9528-bin-win-vulkan-x64/ggml-cpu-x64.dll -lm` |
| **Quick run command** | `cd runner && .\llama_pogls_runner_sid_v2.exe ..\models\SmolLM2-360M.gguf --capture out\test --prompt "hello" --max-new 1` |
| **Full suite command** | `cd runner && .\llama_pogls_runner_sid_v2.exe ..\models\SmolLM2-360M.gguf --capture out\test --prompt "The quick brown fox" --max-new 5` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Quick build check (`gcc -c` only)
- **After every plan wave:** Build runner + run quick capture demo with SmolLM2-360M
- **Before `/gsd-verify-work`:** Full suite on SmolLM2-360M + second GGUF model
- **Max feedback latency:** 60 seconds

---

## Per-task Verification Map

| task ID | Plan | Requirement | Test Type | Automated Command |
|---------|------|-------------|-----------|-------------------|
| 03-01-01 | 01 | DEMO-01 | compile | `gcc -c runner/llama_pogls_runner_sid_v2.c -Irunner -Icollection` |
| 03-01-02 | 01 | DEMO-01 | run | `runner\llama_pogls_runner_sid_v2.exe model.gguf --capture out\test --prompt "hi" --max-new 1` |
| 03-02-01 | 02 | DEMO-01 | compile | `gcc -c runner/capture_store.c -Irunner -Icollection` |
| 03-02-02 | 02 | DEMO-01 | verify | Check `out/test` contains .tw + .gsten files |

---

## Wave 0 Requirements

- [ ] `runner/test_capture.c` — minimal verification helper for capture output
- [ ] Existing infrastructure covers all phase requirements after compilation

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Runner demo on 2nd GGUF model | DEMO-01 | Requires model file not in repo | Run with `qwen2.5-coder-0.5b.gguf --capture out\qwen_test` |
| Lossless verification | DEMO-01 (SC-4) | Visual confirmation of "lossless: ✓" line | Check runner stderr for verification summary |

---

## Validation Sign-Off

- [x] All tasks have verifiable acceptance criteria
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 60s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
