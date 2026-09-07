"""Acquire the fixed native DirectML dependency; prepare local source archives."""
import json
from bootstrap import ROOT, download, extract, sha256

path = download('https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg', ROOT / '.deps/archives/directml.zip')
record = {'package': 'Microsoft.AI.DirectML', 'version': '1.15.4', 'sha256': sha256(path), 'bytes': path.stat().st_size}
lock_path = ROOT / 'native-dependency.lock.json'
if lock_path.exists() and json.loads(lock_path.read_text()) != record:
    raise RuntimeError('DirectML package checksum changed')
lock_path.write_text(json.dumps(record, indent=2) + '\n')
extract(path, ROOT / '.deps/directml')
extract(ROOT / '.deps/archives/json.zip', ROOT / '.deps/json')
