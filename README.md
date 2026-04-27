# FGLS (Geometric File & Library System)

Unified repository for the POGLS/FGLS/LC geometric storage architecture.

## Structure

```
├── core/                    # Core engine (includes Phase 7 continuous development)
│   ├── asset_headers/       # C headers for geometric structures
│   ├── pogls_engine/        # Main POGLS engine
│   └── sessions/            # Session releases (S11, S36, etc.)
├── phases/                  # Sequential development phases
│   ├── phase1-6/           # Initial prototypes
│   └── phase8-12/          # Specialized extensions (FEC, GPU, Hybrid VAE)
├── python_client/           # LC-GCFS Python package
├── experiments/             # GeoPixel visualization experiments
├── tests/                   # Unit & integration tests
├── build/                   # Build scripts
└── docs/                    # Documentation (ROADMAP.md)
```

## Quick Start

### Build Core Engine
```bash
cd build
./build_core.sh
```

### Run Tests
```bash
cd tests
python run_tests.py
```

### Python Client
```bash
pip install -e python_client/
```

## Key Concepts

- **Geometric Addressing**: Data addressed by geometry computed from content itself
- **GCFS**: Geometric Cube File Store (4,896B fixed chunks)
- **Sacred Numbers**: 27 (trit cycle), 6 (dodeca faces), 9 (active cosets), 144 (Fibonacci clock)

## License

MIT
