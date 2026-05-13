# Conflict Resolution

## Duplicate Files (handoff vs src)
- `geo_addr_net.h`
- `geo_compound_cfg.h`
- `geo_tring_walk.h`
- `tgw_cardioid_express.h`
- `tgw_dispatch_v2.h`
- `tgw_ground_lcgw.h`
- `tgw_tri5_wire.h`

## Resolution Policy Used
- `src/` kept as canonical for duplicate names.
- `active_updates/` duplicate headers were not promoted over canonical `src` versions.
- Dependency path normalization was applied in canonical files only.

## Path Normalization Applied
- `src/tgw_dispatch.h`
  - `#include "../pogls_engine/fgls_twin_store.h"` -> `#include "fgls_twin_store.h"`
  - `#include "../pogls_engine/lc_twin_gate.h"` -> `#include "lc_twin_gate.h"`
- `src/tgw_dispatch_v2.h`
  - same normalization as above
- `src/tgw_dispatch_v2_tri5.h`
  - same normalization as above

## Remaining Technical Debt
- `tests/integration/test_rewind_lcgw.c` compiles with format warnings (`%llx` on this GCC/Windows runtime style).
- Warning-only cleanup is safe to do later; compile and link are currently green.
