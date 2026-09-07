"""Cache large build-only wheels with bounded, resumable requests."""
from bootstrap import ROOT, get_json, download
from concurrent.futures import ThreadPoolExecutor
def acquire(pair):
    package, version = pair
    data = get_json(f'https://pypi.org/pypi/{package}/{version}/json')
    wheel = next(x for x in data['urls'] if x['filename'].endswith('win_amd64.whl') and ('cp312-' in x['filename']))
    path = download(wheel['url'], ROOT / '.cache/wheels' / wheel['filename'])
    from bootstrap import sha256
    if sha256(path) != wheel['digests']['sha256']:
        raise RuntimeError(f'Wheel hash mismatch: {package}')
    print(f'Wheel verified: {path.name}', flush=True)

with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(acquire, [('torch','2.10.0'),('pyarrow','23.0.1'),('scipy','1.18.1'),('onnx','1.20.1'),('llvmlite','0.49.0')]))
