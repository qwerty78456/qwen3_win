"""Locate the encoder divergence: official audio tower vs exporter wrapper vs ONNX output.

Build-time diagnostic only. Uses the unmodified official model (FP32, eager) and the
pinned exporter's EncoderWrapper on the same mel, then compares both with the ONNX
encoder output captured by the native runner (traces/native/<fixture>/encoder.f32).
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path
import numpy as np
import torch
from reference import ROOT, CORE
from shipped import REFERENCE_DIR

def load_wrapper_module():
    src = next((ROOT / '.deps/onnx-export').glob('*/src'))
    spec = importlib.util.spec_from_file_location('encoder_wrapper', src / 'encoder_wrapper.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def stats(name, a, b):
    a = np.asarray(a, dtype=np.float32).reshape(-1, a.shape[-1]); b = np.asarray(b, dtype=np.float32).reshape(-1, b.shape[-1])
    err = np.abs(a - b)
    per_token = err.max(1)
    return {'pair': name, 'tokens': int(a.shape[0]), 'max_abs': float(err.max()), 'rmse': float(np.sqrt((err ** 2).mean())),
            'reference_rms': float(np.sqrt((a ** 2).mean())), 'worst_token': int(per_token.argmax()),
            'per_token_max_first_16': [round(float(x), 5) for x in per_token[:16]],
            'per_token_max_last_8': [round(float(x), 5) for x in per_token[-8:]]}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', default='mandarin-0000')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--threads', type=int, default=8)
    args = parser.parse_args()
    torch.set_num_threads(args.threads)
    from transformers_backend import Qwen3ASRForConditionalGeneration
    model = Qwen3ASRForConditionalGeneration.from_pretrained(REFERENCE_DIR, torch_dtype=torch.float32,
                                                             attn_implementation='eager', local_files_only=True).eval()
    tower = model.thinker.audio_tower
    print('audio tower attn implementation:', tower.config._attn_implementation, flush=True)
    mel = np.fromfile(ROOT / 'traces/reference' / args.fixture / 'mel.f32', dtype='<f4').reshape(1, 128, -1)
    frames = mel.shape[2]
    mel_t = torch.from_numpy(mel.copy())
    with torch.inference_mode():
        official = tower(mel_t[0], feature_lens=torch.tensor([frames])).last_hidden_state
        wrapper = load_wrapper_module().EncoderWrapper(tower).eval()
        wrapped = wrapper(mel_t)
        # Official forward with the block-diagonal window mask the code defines but never applies (eager/SDPA path).
        Layer = type(tower.layers[0]); original = Layer.forward
        def patched(self, hidden_states, cu_seqlens, attention_mask=None, **kw):
            if attention_mask is None: attention_mask = tower._prepare_attention_mask(hidden_states, cu_seqlens)
            return original(self, hidden_states, cu_seqlens, attention_mask=attention_mask, **kw)
        Layer.forward = patched
        try: masked = tower(mel_t[0], feature_lens=torch.tensor([frames])).last_hidden_state
        finally: Layer.forward = original
    official = official.detach().cpu().numpy().reshape(-1, official.shape[-1])
    wrapped = wrapped.detach().cpu().numpy().reshape(-1, wrapped.shape[-1])
    masked = masked.detach().cpu().numpy().reshape(-1, masked.shape[-1])
    reference_trace = np.fromfile(ROOT / 'traces/reference' / args.fixture / 'encoder.f32', dtype='<f4').reshape(-1, official.shape[-1])
    native = np.fromfile(ROOT / 'traces/native' / args.fixture / 'encoder.f32', dtype='<f4').reshape(-1, official.shape[-1])
    result = {'fixture': args.fixture, 'mel_frames': frames, 'tokens': int(official.shape[0]), 'comparisons': [
        stats('official_direct vs official_generate_trace', official, reference_trace),
        stats('official vs wrapper', official, wrapped),
        stats('wrapper vs onnx_native', wrapped, native),
        stats('official vs onnx_native', official, native),
        stats('official_with_window_mask vs wrapper', masked, wrapped),
        stats('official_with_window_mask vs onnx_native', masked, native)]}
    print(json.dumps(result, indent=2))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')

if __name__ == '__main__':
    main()
