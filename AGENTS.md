# FGLS Repository Agent Guidelines

## Project Structure
This repository follows the structure outlined in README.md:
- `core/` - Core engine (POGLS, asset headers, sessions)
- `phases/` - Sequential development phases (phase1-6, phase8-12)
- `python_client/` - LC-GCFS Python package
- `experiments/` - GeoPixel visualization experiments

Note: The `tests/`, `build/`, and `docs/` directories mentioned in README were not found in the repository root during inspection.

## Key Commands
### Python Client Installation
```bash
pip install ./python_client/lc_gcfs_pkg
# With optional REST server dependencies:
pip install './python_client/lc_gcfs_pkg[server]'
```

### Building Components
Based on observed Makefiles and build patterns:
- Core engine: `cd core && make`
- Collection components: `cd collection && make`
- Goldberg field core: `cd collection/core/goldberg_field_core && make`
- TPOGLS components: `cd collection/core/pogls_engine/TPOGLS_s11/TPOGLS_s11 && make`

## Testing Approach
Test files are distributed throughout the repository:
- Core tests: `core/pogls_engine/test/` and `core/geo_headers/test/`
- Collection tests: `collection/tests/` and `collection/checkpoint/`
- Phase tests: `phases/*/test_*.c`
- To compile individual test programs, look for accompanying Makefiles or use patterns like:
  ```bash
  gcc -O2 -o test_name test_name.c -lzstd -lm -lpthread
  ```

## Important Notes
- The repository uses geometric addressing and GCFS (Geometric Cube File Store)
- Sacred numbers: 27 (trit cycle), 6 (dodeca faces), 9 (active cosets), 144 (Fibonacci clock)
- Core engine includes Phase 7 continuous development (in core/pogls_engine/)
- Specialized extensions in phases 8-12 (FEC, GPU, Hybrid VAE)
- Many components have their own Makefiles for building
- Look for test_*.c files to find unit tests for specific components
