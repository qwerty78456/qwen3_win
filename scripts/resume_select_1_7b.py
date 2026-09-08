"""Preserve the prior 0.6B evidence and restore the proven 1.7B oracle on user selection."""
import json
import shutil
from bootstrap import ROOT

archive=ROOT/'reports/proof-0.6b-resume'
archive.mkdir(exist_ok=True)
for path in (ROOT/'reports').glob('*.json'):
    if not (archive/path.name).exists(): shutil.copy2(path,archive/path.name)
for name in ['PROOF.md','BENCHMARK.md','COMPATIBILITY.md']:
    if not (archive/name).exists(): shutil.copy2(ROOT/'docs'/name,archive/name)
for name in ['reference','reference-extra']:
    if not (archive/name).exists(): shutil.copytree(ROOT/'regression'/name,archive/name)
if not (archive/'regression-manifest.json').exists(): shutil.copy2(ROOT/'regression/manifest.json',archive/'regression-manifest.json')
baseline=ROOT/'reports/proof-1.7b'
for path in (baseline/'regression-reference').glob('*.json'): shutil.copy2(path,ROOT/'regression/reference'/path.name)
shutil.copy2(baseline/'regression-manifest.json',ROOT/'regression/manifest.json')
config=json.loads((ROOT/'reports/resume-audit/shipped-model-before.json').read_text('utf-8'))
config.update(note='1.7B FP32 retained by explicit user choice during resume audit. DirectML remains under evaluation; see reports/resume-audit and docs/RESUME-AUDIT.md.',default_provider='cpu')
(ROOT/'shipped-model.json').write_text(json.dumps(config,indent=2)+'\n',encoding='utf-8')
print('Selected 1.7B; original 0.6B evidence preserved in',archive)
