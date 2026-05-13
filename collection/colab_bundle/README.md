# Colab Linux Build Bundle

This bundle is the minimal Linux-ready subset for testing the current GeoPixel / TGW integration path in Colab.

## What to copy

Copy the files listed in `FILES.txt` into a Linux workspace that preserves the same relative paths.

## Build

```bash
bash colab_bundle/build_linux.sh
```

## What it builds

- `tests/integration/test_geopixel_session_feed.c`
- `tests/integration/test_startup_trace_payload.c`

Optional extra tests can be added later by extending `build_linux.sh`.

