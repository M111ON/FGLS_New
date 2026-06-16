import struct, json, sys

path = "I:/model/smolVLM-256M-Instruct/model.safetensors"
with open(path, 'rb') as f:
    header_len = struct.unpack('<Q', f.read(8))[0]
    header = json.loads(f.read(header_len))
    keys = sorted(k for k in header if k != '__metadata__')
    print(f"Total tensors: {len(keys)}", flush=True)
    
    if '__metadata__' in header:
        for k, v in header['__metadata__'].items():
            vs = str(v)
            if len(vs) > 200: vs = vs[:200] + '...'
            print(f"  meta[{k}] = {vs}", flush=True)
    
    for k in keys[:30]:
        m = header[k]
        print(f"  {k}: dtype={m.get('dtype','?')} shape={m.get('shape',[])}", flush=True)
    
    print("...", flush=True)
    
    # Count by category
    lm = [k for k in keys if 'vision' not in k]
    vision = [k for k in keys if 'vision' in k]
    print(f"LM tensors: {len(lm)}, Vision tensors: {len(vision)}", flush=True)
    
    # Check model config
    for k in keys:
        if 'model.config' in k:
            print(f"  {k}: {header[k]}", flush=True)
