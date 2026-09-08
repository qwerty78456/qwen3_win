"""Make a model-specific report collection without overwriting historical runs."""
import json
import shutil
from bootstrap import ROOT

dest=ROOT/'reports/current-1.7b'
dest.mkdir(exist_ok=True)
baseline=ROOT/'reports/proof-1.7b'
names=['verification.json','tokenizer.json','preprocessing.json','fixture-frontend.json','speech-gate.json','boundaries.json','memory.json','benchmark-threads.json','encoder-diff-mandarin-0000.json']
for name in names:
    if not (dest/name).exists(): shutil.copy2(baseline/name,dest/name)
for name in ['stages','stages-boundaries','reference-global-attention']:
    if not (dest/name).exists(): shutil.copytree(baseline/name,dest/name)
shutil.copy2(ROOT/'reports/resume-audit/verification-1.7b-cpu.json',dest/'verification.json')
shutil.copy2(ROOT/'reports/cli-failures.json',dest/'cli-failures.json')
for name in ['environment.json','variant-evaluation.json','publisher-hashes.json']:
    shutil.copy2(ROOT/'reports'/name,dest/name)
config=json.loads((ROOT/'shipped-model.json').read_text('utf-8'))
config.update(report_root='reports/current-1.7b',threads=6)
(ROOT/'shipped-model.json').write_text(json.dumps(config,indent=2)+'\n',encoding='utf-8')
print('Current evidence:',dest)
