"""Build qwen3_meta.json from GGUF tensor shapes (avoid broken KV metadata)."""
import json
from gguf import GGUFReader

r = GGUFReader(r'I:\Vault\models\Qwen3-0.6B-Q8_0.gguf')
tensors = {t.name: list(t.shape) for t in r.tensors}

n_layers = len([n for n in tensors if 'attn_q.weight' in n])
q_shape = tensors['blk.0.attn_q.weight']
k_shape = tensors['blk.0.attn_k.weight']
gate_shape = tensors['blk.0.ffn_gate.weight']
embed_shape = tensors['token_embd.weight']

meta = {
    'model': 'Qwen3-0.6B',
    'n_layers': int(n_layers),
    'dim': int(q_shape[0]),
    'ffn_dim': int(gate_shape[1]),
    'head_dim': 128,
    'n_heads': int(q_shape[1] // 128),
    'n_kv_heads': int(k_shape[1] // 128),
    'vocab_size': int(embed_shape[1]),
}
with open('build/qwen3_meta.json', 'w') as f:
    json.dump(meta, f, indent=2)
print(json.dumps(meta, indent=2))
