# Build And Test

## Prerequisites
- `gcc` available in PATH
- Workspace root: `I:\FGLS_new\collection`

## Canonical Integration Compile Command
```powershell
gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_dispatch_v2.c -o build/integration/test_dispatch_v2.exe
gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_ground_lcgw.c -o build/integration/test_ground_lcgw.exe
gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_rewind_lcgw.c -o build/integration/test_rewind_lcgw.exe
gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_metatron_route.c -o build/integration/test_metatron_route.exe
gcc -O2 -Wall -Wextra -Isrc -Icore/pogls_engine -Icore/core tests/integration/test_frustum_wire.c -o build/integration/test_frustum_wire.exe
```

## Optional Make-style Entry
- File: `Makefile.integration`
- If your environment has `make`:
```bash
make -f Makefile.integration
```

## Validation Result (current workspace)
- Compile OK:
  - `test_dispatch_v2`
  - `test_ground_lcgw`
  - `test_rewind_lcgw`
  - `test_metatron_route`
  - `test_frustum_wire`
- Notes:
  - `test_rewind_lcgw` still emits warning-level format diagnostics on this toolchain.
