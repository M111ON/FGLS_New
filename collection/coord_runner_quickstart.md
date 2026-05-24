# Coordinate Runner Quickstart

## 1. Build stores

Build each model store first, then register it in the registry.

## 2. Resolve a coordinate

```bash
python python_src/coord_runtime.py coord_registry.example.json resolve 2 S
```

## 3. Prime a coordinate

```bash
python python_src/coord_runtime.py coord_registry.example.json prime 2 S
```

## 4. Run the C runner by coordinate

```bash
core\pogls_runner.exe --coord coord_registry.example.json 2 S 32 "hello"
```

## 5. Resolve from runner only

```bash
core\pogls_runner.exe --coord-resolve coord_registry.example.json 2 S
```

## 6. Inspect a text probe

```bash
python python_src/coord_runtime.py coord_real_registry.json inspect-text qwen "hello world"
```

## 7. Inspect a hidden-state probe

```bash
python python_src/coord_runtime.py coord_real_registry.json inspect-npy qwen probe.npy
```

## Notes

- `gguf_path` is required for runner resolution.
- `store_path` is the geometry store prefix without suffix.
- `ns` is optional and defaults to `null`.
- `force_cpu: true` disables GPU layers for that model.
