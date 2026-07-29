# pogls_addr.c

> 🟢 **[ACTIVE]** — This file is in current use.

**Module:** `pogls_core`  
**Path:** `pogls_core/pogls_addr.c`  
**Status:** `active`  
**Note:** modified 17d ago  
**Generated:** 2026-07-29 14:58  

## API Functions

- `uint64_t pogls_tier_capacity(uint8_t tier)`
- `uint8_t pogls_select_tier(uint32_t tensor_count, uint32_t hidden_dim)`
- `PoglsAddrDecomp pogls_decompose(uint32_t addr, uint8_t tier)`
- `uint32_t pogls_compose(uint32_t macro, uint32_t micro, uint8_t tier)`
- `PoglsGeoDecomp pogls_to_geo(uint32_t addr, uint8_t tier)`
- `uint32_t pogls_from_name(const char *name, uint8_t tier)`
- `return pogls_compose(macro, micro, tier)`
- `uint32_t pogls_capo(uint32_t base, uint32_t face, uint8_t tier)`
- `int pogls_addr_valid(uint32_t addr, uint8_t tier)`
- `void pogls_addr_print(uint32_t addr, uint8_t tier)`

