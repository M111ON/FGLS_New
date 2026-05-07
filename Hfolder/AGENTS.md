# Repository Guidelines

## Project Structure & Module Organization
This repository is a small, file-based GeoVault / GeoPixel toolset. Core C sources live at the repo root, including `geo_vault.h`, `geo_vault_cli.c`, `geo_vault_fuse.c`, `geo_vault_bench.c`, `geo_vault_test.c`, and `geopixel_v18.c` / `geopixel_v19.c`. Supporting headers such as `geo_vault_io.h`, `geo_gpx_decode.h`, and `gpx4_container.h` sit beside them. `geo_gpx_visualizer.html` is the standalone browser visualizer, and `document.txt` / `SESSION_HANDOFF_O9.md` are reference notes.

## Build, Test, and Development Commands
There is no package manager or build system in this folder; compile the target directly with `gcc`.

- `gcc -O2 -o geo_vault_test geo_vault_test.c -lzstd` builds the round-trip test binary.
- `gcc -O2 -o gvault geo_vault_cli.c -lzstd` builds the CLI pack/unpack tool.
- `gcc -O3 -I. -o geopixel_v18 geopixel_v18.c -lm -lzstd -lpthread -lpng` builds the main GeoPixel encoder/decoder.
- Open `geo_gpx_visualizer.html` in a browser to inspect `.gpx` files interactively.

## Coding Style & Naming Conventions
Use C99-style C with 4-space indentation, `snake_case` for functions and variables, and `UPPER_SNAKE_CASE` for constants and macros. Keep headers self-contained and prefer fixed-width integer types (`uint32_t`, `uint64_t`) for file-format code. In the HTML visualizer, keep CSS and JavaScript inline unless a split file is clearly necessary.

## Testing Guidelines
The repo uses self-contained C test programs rather than a unit-test framework. Favor deterministic, reproducible checks: round-trip pack/unpack, hash comparison, and decode verification against known sample files. Name new checks by feature, for example `geo_vault_test.c` or `gpx_decode_test.c`, and document the exact compile command in a file comment when adding a new executable.

## Commit & Pull Request Guidelines
This folder does not contain Git history, so no local commit convention can be inferred here. Use short, imperative commit messages with a clear scope, for example `geo_vault: fix frame seek table`. Pull requests should describe the behavior change, list the commands used to verify it, and include screenshots or sample outputs when touching `geo_gpx_visualizer.html` or file-format behavior.

## Agent-Specific Notes
Prefer editing the existing root files rather than introducing new subdirectories. Keep changes minimal and format-preserving, and update the relevant handoff note when a change affects on-disk behavior or file layout.
