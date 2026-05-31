"""Full parse of Qwen3.5-9B-DeepSeek-V4-Flash with 20MB range."""
from peek_gguf_remote import parse_gguf_remote

URL = "https://huggingface.co/Jackrong/Qwen3.5-9B-DeepSeek-V4-Flash-GGUF/resolve/main/Qwen3.5-9B-DeepSeek-V4-Flash-Q4_K_M.gguf"
meta = parse_gguf_remote(URL, max_bytes=20*1024*1024)

print(f"Architecture:  {meta.get('general.architecture', '?')}")
print(f"File type:     {meta.get('general.file_type', '?')}")
print(f"Layers:        {meta.get('qwen35.block_count', '?')}")
print(f"Dim:           {meta.get('qwen35.embedding_length', '?')}")
print(f"FFN:           {meta.get('qwen35.feed_forward_length', '?')}")
print(f"Heads:         {meta.get('qwen35.attention.head_count', '?')}")
print(f"KV heads:      {meta.get('qwen35.attention.head_count_kv', '?')}")
print(f"Key length:    {meta.get('qwen35.attention.key_length', '?')}")
print(f"Value length:  {meta.get('qwen35.attention.value_length', '?')}")
print(f"Context:       {meta.get('qwen35.context_length', '?')}")
print(f"Rope freq:     {meta.get('qwen35.rope.freq_base', '?')}")
print(f"Rope dim cnt:  {meta.get('qwen35.rope.dimension_count', '?')}")
print(f"Rope sections: {meta.get('qwen35.rope.dimension_sections', '?')}")
print(f"Full attn int:{meta.get('qwen35.full_attention_interval', '?')}")
print()

# SSM
print("=== SSM Config ===")
for k in sorted(meta.keys()):
    if k.startswith("qwen35.ssm."):
        print(f"  {k} = {meta[k]}")

print()
print("=== Comparison Qwen2.5-0.5B vs Qwen3.5-9B ===")
print(f"  {'':30s} {'Qwen2.5-0.5B':15s} {'Qwen3.5-9B'}")
print(f"  {'─'*60}")
print(f"  {'Architecture':30s} {'qwen2':15s} {'qwen35'}")
print(f"  {'Layers':30s} {'24':15s} {'32'}")
print(f"  {'Dim':30s} {'896':15s} {'4096'}")
print(f"  {'FFN':30s} {'4864':15s} {'12288'}")
print(f"  {'Heads':30s} {'14':15s} {'16'}")
print(f"  {'KV heads':30s} {'2':15s} {'4'}")
print(f"  {'Head dim':30s} {'64':15s} {'256 (key_len)'}")
print(f"  {'Context':30s} {'32K':15s} {'262K'}")
print(f"  {'Rope freq':30s} {'1M':15s} {'10M'}")
print(f"  {'Rope sections':30s} {'-':15s} {'[11,11,10,0]'}")
print(f"  {'SSM':30s} {'-':15s} {'conv=4 state=128'}")
print(f"  {'Full attn interval':30s} {'every':15s} {'every 4th layer'}")
print(f"  {'Tensors':30s} {'291':15s} {'427'}")
print(f"  {'Tokens (vocab)':30s} {'151,936':15s} {'248,320'}")
print(f"  {'Merges':30s} {'151,387':15s} {'247,587'}")
print(f"  {'KV section size':30s} {'~5.7 MB':15s} {'~10.4 MB'}")
print(f"  {'Quant':30s} {'Q8_0':15s} {'Q4_K_M'}")
print(f"  {'Size on disk':30s} {'644 MB':15s} {'~5.5 GB'}")
