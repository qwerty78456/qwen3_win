"""Acquire the pinned official Qwen/Qwen3-ASR-0.6B checkpoint (oracle for the 0.6B configuration).

Pins the revision and publisher LFS hashes into remote-assets.lock.json ("reference_0_6b") and
downloads the safetensors, configs, tokenizer files and chat template into reference-model-0.6b/.
Build-time only.
"""
import json
import sys
from bootstrap import ROOT, download, get_json, sha256

REPO = 'Qwen/Qwen3-ASR-0.6B'
lock_path = ROOT / 'remote-assets.lock.json'
lock = json.loads(lock_path.read_text('utf-8'))
if 'reference_0_6b' not in lock:
    data = get_json(f'https://huggingface.co/api/models/{REPO}?blobs=true')
    files = [{'path': s['rfilename'], 'bytes': s.get('lfs', {}).get('size', s.get('size')), 'sha256': s.get('lfs', {}).get('sha256')}
             for s in data['siblings'] if s['rfilename'].endswith(('.json', '.safetensors', '.txt', '.jinja')) or s['rfilename'] == 'README.md']
    lock['reference_0_6b'] = {'repo': REPO, 'revision': data['sha'], 'files': files}
    lock_path.write_text(json.dumps(lock, indent=2) + '\n', encoding='utf-8')
    print('Pinned', REPO, data['sha'], flush=True)
info = lock['reference_0_6b']
target = ROOT / 'reference-model-0.6b'
for asset in info['files']:
    path = download(f"https://huggingface.co/{info['repo']}/resolve/{info['revision']}/{asset['path']}", target / asset['path'])
    digest = sha256(path)
    if asset['sha256'] and (path.stat().st_size != asset['bytes'] or digest != asset['sha256']):
        print('HASH MISMATCH', asset['path'], digest, flush=True); sys.exit(1)
    print('verified', asset['path'], path.stat().st_size, digest[:16], flush=True)
print('0.6B reference checkpoint ready', flush=True)
