# Stack Stitch Map (No Refactor)

This workspace is spread across multiple roots, but can be operated as one stack.

## Active Pieces

- Backend core: `I:\ZGLS\smart_folder_mvp`
  - API: `smart_folder_mvp.api`
  - Service/store: `service.py`, `store.py`
- Bond/LC bridge modules: `I:\FGLS_new\collection\python_src`
  - `pogls_bridge.py`, `lc_client.py`, `geo_field_bridge.py`
  - `storage_cleaner.pyw` (maintenance/cleanup)
- Operator UI: `im_anywhere_v2.html`
  - drop/preview/apply, vault, dependency scan/resolve

## One-Command Boot

From PowerShell:

```powershell
pwsh -File I:\ZGLS\smart_folder_mvp\start_stack.ps1
```

This does:

1. sets `SMART_FOLDER_ROOT=I:\FGLS_new\collection`
2. starts API (`uvicorn smart_folder_mvp.api:app --factory`) on `127.0.0.1:8788`
3. opens API docs at `/docs`
4. opens `im_anywhere_v2.html` if found

## Recommended Daily Flow

1. Drop files/zips from operator UI
2. Review dependency panel + resolve missing
3. Apply safe updates
4. Run storage cleaner as maintenance pass (`storage_cleaner.pyw`)

## Why this works

- No migration/restructure needed now
- Keeps random-access workflow intact
- Gives one stable entrypoint to operate all pieces together
