#!/usr/bin/env python3
"""Write demos/pat/data/weights.bin from the network released with "Points as Tori".

    git clone https://github.com/nzfeng/points-as-tori
    pip install jax flax
    scripts/pat_export_weights.py points-as-tori/src/pointsastori/models/FundamentalFormPredictor.pkl

The file is "PATW", five uint32 (embedding width, blocks, heads, MLP width,
neighbours), then every weight as float32 in the order pat_net_forward() in
demos/pat/method.c reads them. Kernels stay [input][output], as flax has them.
"""
import pickle
import struct
import sys

import jax  # noqa: F401  (the pickle holds jax arrays)
import numpy as np

saved = pickle.load(open(sys.argv[1], "rb"))
params = {path: np.asarray(getattr(v, "value", v), np.float32) for path, v in saved["params"].flat_state()}
embed, layers, heads, mlp, k = (saved[n] for n in ("embed_dim", "n_layers", "n_heads", "mlp_dim", "k_neighbors"))

out = []
def put(*path, shape=None):
    a = params[path]
    out.append(np.ascontiguousarray(a if shape is None else a.reshape(shape)).ravel())

for name in ("kernel", "bias"):
    put("input_encoder", "mlp", "layers", 0, name)
for name in ("scale", "bias"):
    put("input_encoder", "mlp", "layers", 1, name)
for b in range(layers):
    put("blocks", b, "norm1", "scale"); put("blocks", b, "norm1", "bias")
    for part in ("query", "key", "value", "out"):
        put("blocks", b, "attention", part, "kernel", shape=(embed, embed))   # heads side by side
        put("blocks", b, "attention", part, "bias", shape=(embed,))
    put("blocks", b, "norm2", "scale"); put("blocks", b, "norm2", "bias")
    for layer in (0, 3):
        put("blocks", b, "ffn", "layers", layer, "kernel"); put("blocks", b, "ffn", "layers", layer, "bias")
put("final_norm", "scale"); put("final_norm", "bias")
put("mlp", "layers", 0, "kernel"); put("mlp", "layers", 0, "bias")

weights = np.concatenate(out)
with open("demos/pat/data/weights.bin", "wb") as f:
    f.write(b"PATW")
    f.write(struct.pack("<5I", embed, layers, heads, mlp, k))
    f.write(weights.tobytes())
print(f"demos/pat/data/weights.bin: {len(weights)} weights; width {embed}, {layers} blocks, {heads} heads, MLP {mlp}, {k} neighbours")
