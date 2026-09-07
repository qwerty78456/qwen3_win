"""Build-time-only dependency acquisition. Nothing in the app downloads assets."""
import concurrent.futures as cf
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time
import urllib.request
import zipfile

if sys.stdout is not None:
    sys.stdout.reconfigure(encoding='utf-8')

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'dependencies.lock.json'
HEADERS = {'User-Agent': 'AsrWin-build/0.1'}

def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=60) as r:
        return json.load(r)

def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(8 * 1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()

def download(url, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        return dest
    partial = dest.with_name(dest.name + '.part')
    for attempt in range(4):
        try:
            offset = partial.stat().st_size if partial.exists() else 0
            headers = dict(HEADERS, Range='bytes=0-0')
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
                content_range = r.headers.get('Content-Range', '')
                total = int(content_range.rsplit('/', 1)[1]) if r.status == 206 else None
                if total is None:
                    with partial.open('wb') as f:
                        shutil.copyfileobj(r, f, 256 * 1024)
                else:
                    r.read()
            if total is not None:
                if offset > total:
                    raise ValueError('Partial file exceeds remote size')
                print(f'Downloading {dest.relative_to(ROOT)} ({offset}/{total} bytes)', flush=True)
                def fetch_range(start):
                    end = min(start + 8 * 1024 * 1024, total) - 1
                    headers = dict(HEADERS, Range=f'bytes={start}-{end}')
                    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
                        if r.status != 206 or r.headers.get('Content-Range') != f'bytes {start}-{end}/{total}':
                            raise ValueError('Server did not honor exact requested range')
                        chunk = r.read()
                    if len(chunk) != end - start + 1:
                        raise ValueError('Incomplete range response')
                    return chunk
                with partial.open('ab') as f, cf.ThreadPoolExecutor(max_workers=4) as ranges:
                    while offset < total:
                        starts = range(offset, min(total, offset + 32 * 1024 * 1024), 8 * 1024 * 1024)
                        # Commit only contiguous completed ranges, so interrupted runs resume safely.
                        for chunk in ranges.map(fetch_range, starts):
                            f.write(chunk)
                            f.flush()
                            offset += len(chunk)
                            if offset % (256 * 1024 * 1024) == 0 or offset == total:
                                print(f'  {dest.name}: {offset}/{total}', flush=True)
            partial.replace(dest)
            return dest
        except Exception as e:
            print(f'Retry {attempt + 1}: {dest.name}: {e}', flush=True)
            if attempt == 3:
                raise
            time.sleep(2 ** attempt)

def extract(archive, dest):
    if (dest / '.extracted').exists():
        return
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            target = (dest / entry.filename).resolve()
            if not target.is_relative_to(dest.resolve()):
                raise ValueError('Archive path escapes destination')
        z.extractall(dest)
    (dest / '.extracted').write_text('ok', encoding='utf-8')

def main():
    if LOCK.exists():
        lock = json.loads(LOCK.read_text(encoding='utf-8'))
    else:
        lock = {'schema': 1, 'onnxruntime': {'package': 'Microsoft.ML.OnnxRuntime.DirectML', 'version': '1.24.4'}, 'sources': {}, 'models': {}, 'files': {}}
        for key, repo in {'onnx-export': 'andrewleech/qwen3-asr-onnx', 'tokenizers-cpp': 'mlc-ai/tokenizers-cpp', 'kissfft': 'mborgerding/kissfft', 'libfvad': 'dpirch/libfvad', 'json': 'nlohmann/json', 'qwen-reference': 'QwenLM/Qwen3-ASR'}.items():
            commit = get_json(f'https://api.github.com/repos/{repo}/commits?per_page=1')[0]['sha']
            lock['sources'][key] = {'repo': repo, 'revision': commit}
        for key, repo in {'onnx': 'andrewleech/qwen3-asr-1.7b-onnx', 'reference': 'Qwen/Qwen3-ASR-1.7B'}.items():
            data = get_json(f'https://huggingface.co/api/models/{repo}')
            lock['models'][key] = {'repo': repo, 'revision': data['sha'], 'files': [v['rfilename'] for v in data['siblings']]}
        LOCK.write_text(json.dumps(lock, indent=2) + '\n', encoding='utf-8')

    tasks = []
    for key, info in lock['sources'].items():
        url = f"https://codeload.github.com/{info['repo']}/zip/{info['revision']}"
        tasks.append((url, ROOT / '.deps' / 'archives' / f'{key}.zip'))
    version = lock['onnxruntime']['version']
    tasks.append((f'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/{version}/microsoft.ml.onnxruntime.directml.{version}.nupkg', ROOT / '.deps' / 'archives' / 'onnxruntime.zip'))
    required = {'encoder.onnx', 'decoder_init.onnx', 'decoder_step.onnx', 'decoder_weights.data', 'embed_tokens.bin', 'config.json', 'tokenizer.json', 'tokenizer_config.json', 'preprocessor_config.json', 'added_tokens.json', 'vocab.json', 'README.md'}
    for key, info in lock['models'].items():
        for name in info['files']:
            use = name in required if key == 'onnx' else (name.endswith(('.json', '.safetensors', '.txt', '.jinja')) or name == 'README.md')
            if use:
                tasks.append((f"https://huggingface.co/{info['repo']}/resolve/{info['revision']}/{name}", ROOT / ('models/qwen3-asr-1.7b-fp32' if key == 'onnx' else 'reference-model') / name))
    failures = []
    with cf.ThreadPoolExecutor(max_workers=4) as pool:
        futures = {pool.submit(download, url, dest): dest for url, dest in tasks}
        for future in cf.as_completed(futures):
            dest = futures[future]
            try:
                path = future.result()
                digest = sha256(path)
                rel = path.relative_to(ROOT).as_posix()
                expected = lock['files'].get(rel)
                record = {'sha256': digest, 'bytes': path.stat().st_size}
                if expected and expected != record:
                    raise ValueError(f'Pinned checksum mismatch: {rel}')
                lock['files'][rel] = record
                LOCK.write_text(json.dumps(lock, indent=2) + '\n', encoding='utf-8')
                print(f'Verified {rel}: {record["bytes"]} bytes', flush=True)
            except Exception as e:
                failures.append(str(e))
    for key in lock['sources']:
        archive = ROOT / '.deps' / 'archives' / f'{key}.zip'
        if archive.exists():
            extract(archive, ROOT / '.deps' / key)
    archive = ROOT / '.deps' / 'archives' / 'onnxruntime.zip'
    if archive.exists():
        extract(archive, ROOT / '.deps' / 'onnxruntime')
    if failures:
        raise RuntimeError('\n'.join(failures))
    print('All pinned assets acquired and hashed.', flush=True)

if __name__ == '__main__':
    main()
