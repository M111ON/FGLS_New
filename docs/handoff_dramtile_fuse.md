# Handoff — DRamTile FUSE Mount (in progress)

## Goal of next session
Complete the DRamTile FUSE mount for Windows via WinFsp. Get `dramtile_mount.exe` working so a DRamTile twin file can be mounted as a directory, letting users `cat`, `ls`, `cp` tensors as read-only files.

## State of play

### Done
- **DRamTile library split**: `dramtile_store.h` (declarations + inline hot-path functions) → `dramtile_store.c` (implementations, compiled once) → `libdramtile.a`. 12 consumers include `.h` unchanged but must link the .a.
- **WinFsp import library created**: `runner/libwinfsp-x64.a` (from `winfsp-x64.dll` via `gendef` + `dlltool`)
- **FUSE mount skeleton** written: `runner/dramtile_fuse.c` compiles cleanly with WinFsp FUSE 2 API.
- **Test twin file** created: `test_model.dramtile` with 9 tensors via `create_test_twin.c`

### Blocking
1. **Runtime DLL load**: `dramtile_mount.exe` exits with 0xC0000135 (STATUS_DLL_NOT_FOUND) unless `winfsp-x64.dll` is in PATH. Solved for testing by setting `$env:PATH` but needs a proper solution.
2. **"mount point in use"**: WinFsp reports mount point in use for any directory. May need to unmount previous ghost mounts first (via `launchctl-x64.exe stop FUSE ...` or `fsptool-x64.exe`). Or use a fresh mount point path each time.
3. **"0 tensors" on reopen**: `dt_store_init_twin(&store, path, 0)` with `max_bytes=0` causes `CreateFileMappingA(..., 0, 0)` to fail or map 0 bytes. Windows path in `dt_store_init_twin` does NOT update `cap` from existing file size (unlike Linux path which sets `cap = st.st_size`). Workaround: pass `1GB` as max_bytes. But the real bug is in the Windows path of `dt_store_init_twin`.
4. **`test_dramtile_twin.c` crash**: Pre-existing crash at Phase 3 (memset on NULL from dt_get). Not caused by library split — minimal tests pass fine. Suspect a hash table offset mismatch, needs GDB debug if important.

## Open decisions
- Should `dramtile_mount.exe` embed the WinFsp DLL search path (e.g., via `SetDllDirectory` at startup)?
- Read-only vs read-write mount? Currently read-only implemented. Write would need `dt_put` on file close.
- Where to deploy: standalone `.exe` in `runner/` or as part of a larger vault tool?

## Skills to use
- `msys2-build-pipeline` — for MinGW compilation on Windows with WinFsp linking
- `cross-session-board` — to post/track FUSE mount progress cards

## Artifacts
- `runner/dramtile_fuse.c` — FUSE mount implementation (167 lines, compiles clean)
- `runner/dramtile_store.h` — DRamTile public API (declarations + inline hot-path)
- `runner/dramtile_store.c` — DRamTile implementation
- `runner/libwinfsp-x64.a` — WinFsp MinGW import library
- `test_model.dramtile` — test twin file with 9 tensors
- `runner/create_test_twin.c` — creates test twin file
- Prior handoff files in `docs/` for full project context