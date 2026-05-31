"""Peek GGUF metadata — remotely (header-only Range) or locally."""
import struct, os, sys, gguf

def peek(path_or_repo):
    if "/" in path_or_repo and "\\" not in path_or_repo and not os.path.exists(path_or_repo):
        from huggingface_hub import hf_hub_download as hdl
        parts = path_or_repo.split("/")
        repo_id = "/".join(parts[:2])
        fname = "/".join(parts[2:])
        path = hdl(repo_id, fname)
        print(f"Remote → {path}")
    else:
        path = path_or_repo

    reader = gguf.GGUFReader(path)
    arch = reader.fields["general.architecture"].parts[-1].item().decode()
    print(f"\n=== {arch} GGUF Metadata ===")
    print(f"File: {os.path.getsize(path):,} bytes")
    print(f"Tensors: {len(reader.tensors)}")

    # Architecture-aware key prefix
    for key in sorted(reader.fields.keys()):
        field = reader.fields[key]
        v = field.parts[-1]
        if hasattr(v, "item"):
            v = v.item()
            if isinstance(v, bytes):
                v = v.decode("utf-8", errors="replace")
        elif hasattr(v, "tolist"):
            v = v.tolist()
            if len(v) > 8:
                v = v[:8] + ["..."]
        print(f"  {key} = {v}")


if __name__ == "__main__":
    arg = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Administrator.AVENTADOR\.cache\huggingface\hub\models--Qwen--Qwen2.5-0.5B-Instruct-GGUF\snapshots\9217f5db79a29953eb74d5343926648285ec7e67\qwen2.5-0.5b-instruct-q8_0.gguf"
    peek(arg)
