import os, json, glob

root = "I:/FGLS_new"
incs = [
    ".",
    "../collection",
    "../collection/src",
    "../collection/core",
    "../collection/core/core",
    "../collection/geopixel",
    "../collection/geopixel/Metatron/core",
    "../collection/pogls_engine",
    "../collection/geo_jump_module/include",
    "../collection/geopixel/hbv_bundle/Diamond_shell_encoder",
    "../collection/geopixel/hbv_bundle/Diamond_decode_hamburger",
    "../collection/geopixel/hbv_bundle/core",
    "../collection/Hfolder",
    "../collection/rdh",
    "I:/llama.cpp/include",
    "I:/llama.cpp/ggml/include",
    "I:/llama.cpp/src",
    "pogls_core",
]
inc_args = [f"-I{d}" for d in incs]

cflags_c = " ".join(["-O2", "-std=c11", "-fno-strict-aliasing"] + inc_args)
cflags_cxx = " ".join(["-O2", "-std=c++17"] + inc_args)

entries = []
seen = set()

for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if not d.startswith(".") and d != "deprecated"]
    for f in filenames:
        if f.endswith((".c", ".cpp")):
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if rel in seen:
                continue
            seen.add(rel)
            is_cxx = f.endswith(".cpp")
            entry = {
                "directory": root,
                "file": rel,
                "command": f"gcc {cflags_cxx if is_cxx else cflags_c} -c -o {f}.o {rel}",
            }
            entries.append(entry)

with open(os.path.join(root, "compile_commands.json"), "w") as f:
    json.dump(entries, f, indent=2)

print(f"Generated compile_commands.json with {len(entries)} entries")
