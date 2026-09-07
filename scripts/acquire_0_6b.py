"""Acquire the pinned Qwen3-ASR-0.6B FP32 ONNX export for the model-configuration evaluation.

Pins the repository revision and publisher LFS SHA-256 values into remote-assets.lock.json
(key "onnx_0_6b") on first use, downloads the FP32 files into models/qwen3-asr-0.6b-fp32/, and
reuses the processor-generated mel filters and prompt reference when the tokenizer is identical
to the 1.7B export. Build-time only; never part of the package unless the configuration is chosen.
"""
import json
import shutil
import sys
from bootstrap import ROOT, HEADERS, download, get_json, sha256

REPO = 'andrewleech/qwen3-asr-0.6b-onnx'
FILES = ['config.json', 'tokenizer.json', 'encoder.onnx', 'decoder_init.onnx', 'decoder_step.onnx', 'decoder_weights.data', 'embed_tokens.bin', 'README.md']
lock_path = ROOT / 'remote-assets.lock.json'
lock = json.loads(lock_path.read_text('utf-8'))
if 'onnx_0_6b' not in lock:
    data = get_json(f'https://huggingface.co/api/models/{REPO}?blobs=true')
    files = []
    for s in data['siblings']:
        if s['rfilename'] in FILES:
            files.append({'path': s['rfilename'], 'bytes': s.get('lfs', {}).get('size', s.get('size')), 'sha256': s.get('lfs', {}).get('sha256')})
    lock['onnx_0_6b'] = {'repo': REPO, 'revision': data['sha'], 'files': files}
    lock_path.write_text(json.dumps(lock, indent=2) + '\n', encoding='utf-8')
    print('Pinned', REPO, data['sha'], flush=True)
info = lock['onnx_0_6b']
target = ROOT / 'models/qwen3-asr-0.6b-fp32'
target.mkdir(parents=True, exist_ok=True)
for asset in info['files']:
    path = download(f"https://huggingface.co/{info['repo']}/resolve/{info['revision']}/{asset['path']}", target / asset['path'])
    digest = sha256(path)
    if asset['sha256'] and (path.stat().st_size != asset['bytes'] or digest != asset['sha256']):
        print('HASH MISMATCH', asset['path'], digest, flush=True); sys.exit(1)
    print('verified', asset['path'], path.stat().st_size, digest[:16], flush=True)
same_tokenizer = sha256(target / 'tokenizer.json') == sha256(ROOT / 'models/qwen3-asr-1.7b-fp32/tokenizer.json')
print('tokenizer identical to 1.7B export:', same_tokenizer, flush=True)
if not same_tokenizer:
    sys.exit('Tokenizer differs; regenerate prompt_reference.json for this model before use')
for name in ['mel_filters.bin', 'prompt_reference.json']:
    shutil.copyfile(ROOT / 'models/qwen3-asr-1.7b-fp32' / name, target / name)
print('0.6B assets ready', flush=True)
