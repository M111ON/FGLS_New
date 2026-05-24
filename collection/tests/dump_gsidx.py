import struct
with open("build/qwen_geom_v3.gsidx", "rb") as f:
    hdr = f.read(32)
    print(f"Header ({len(hdr)}B): {hdr.hex()}")
    magic = hdr[:8]
    n_entries = struct.unpack_from("<I", hdr, 8)[0]
    data_size = struct.unpack_from("<Q", hdr, 12)[0]
    pad = hdr[20:32]
    print(f"  magic={magic} n_entries={n_entries} data_size={data_size} pad={pad.hex()}")
    print()
    entries = []
    for i in range(n_entries):
        raw = f.read(20)
        if len(raw) < 20: break
        z, si, off, n_rows, n_cols = struct.unpack_from("<HHqII", raw, 0)
        entries.append((z, si, off, n_rows, n_cols))
        if i < 5 or i >= n_entries - 3:
            print(f"  [{i}] zone={z} shape_idx={si} offset={off} rows={n_rows} cols={n_cols}")
    print(f"  ... {len(entries)} total entries")
    
    shapes = ['I','O','T','S','Z','L']
    ns_names = {0:'raw',12:'Q',24:'K',36:'V',48:'O',60:'G',72:'U',84:'D'}
    
    zone_keys = set()
    for z, si, off, nr, nc in entries:
        # decode namespace
        ns_found = ''
        base_zone = z
        for ns_shift, ns_name in sorted(ns_names.items()):
            if ns_shift > 0 and z >= ns_shift and z < ns_shift + 12:
                ns_found = ns_name
                base_zone = z - ns_shift
                break
        shape_char = shapes[si] if si < len(shapes) else '?'
        zone_keys.add((ns_found, base_zone, shape_char))
    
    print(f"\nUnique keys: {len(zone_keys)}")
    for ns_name in sorted(ns_names.values()):
        for z in range(12):
            for s in shapes:
                if (ns_name, z, s) in zone_keys:
                    print(f"  ns={ns_name:4s} zone={z:2d} shape={s}")
    print(f"\nTotal: {len(entries)} entries, {len(zone_keys)} unique keys")
