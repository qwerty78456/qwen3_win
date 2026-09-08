"""Synthetic corruption tests for the numerical proof checker, no ASR model needed."""
import json
import subprocess
import sys
import numpy as np
from bootstrap import ROOT

fixture='_validator-self-test'
ref=ROOT/'traces/reference'/fixture
native=ROOT/'traces/native'/fixture
for path in [ref,native]:
    path.mkdir(parents=True,exist_ok=True)
    (path/'prompt.json').write_text(json.dumps({'ids':[1],'positions':[0]}),encoding='utf-8')
    (path/'result.json').write_text(json.dumps({'tokens':[151645],'completion':'eos','cache_steps':[{'keys':[1,1,1,1,1],'values':[1,1,1,1,1]}]}),encoding='utf-8')
    for name in ['pcm','mel','encoder','input_embeds','logits_0','keys_0','values_0']:
        np.zeros(8,dtype='<f4').tofile(path/(name+'.f32'))
rows=[]
def run(name,expected):
    report=ROOT/'reports/resume-audit'/('validator-'+name+'.json')
    process=subprocess.run([sys.executable,str(ROOT/'scripts/compare_traces.py'),fixture,'--report',str(report)],stdout=subprocess.DEVNULL,timeout=30)
    rows.append({'name':name,'exit_code':process.returncode,'success':(process.returncode==0)==expected})
run('valid',True)
path=native/'mel.f32';original=path.read_bytes();path.unlink();run('missing-stage',False);path.write_bytes(original)
bad=np.zeros(8,dtype='<f4');bad[0]=np.nan;bad.tofile(path);run('nonfinite',False);path.write_bytes(original)
np.ones(8,dtype='<f4').tofile(path);run('numerical-divergence',False);path.write_bytes(original)
path=native/'result.json';original=path.read_bytes();body=json.loads(original);body['tokens']=[151643];path.write_text(json.dumps(body),encoding='utf-8');run('token-mismatch',False);path.write_bytes(original)
body=json.loads(original);body['completion']='token_limit';path.write_text(json.dumps(body),encoding='utf-8');run('incomplete-generation',False);path.write_bytes(original)
path=native/'prompt.json';original=path.read_bytes();path.write_text('{}',encoding='utf-8');run('prompt-mismatch',False);path.write_bytes(original)
result={'success':all(x['success'] for x in rows),'cases':rows}
(ROOT/'reports/resume-audit/validator-failures.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['success'] else 1)
