"""Exercise invalid input and missing proof assets through the shipped executable.

Every case must fail: nonzero exit, `success: false` and a non-empty error in the report. The DirectML cases
depend on whether the build is validated (shipped-model.json directml.validated): an unvalidated build must
reject `--provider directml` without the experimental flag and an explicit adapter; a validated build accepts
those, so only the adapter-validation cases remain.
"""
import json
from pathlib import Path
import struct
import subprocess
from bootstrap import ROOT

folder=ROOT/'traces/cli-failures'
folder.mkdir(parents=True,exist_ok=True)
exe=ROOT/'build/Release/AsrWin.exe'
from shipped import MODEL_DIR, DIRECTML_VALIDATED
model=MODEL_DIR
experimental=[] if DIRECTML_VALIDATED else ['--experimental-directml']
def wav(fmt,data):
    body=b'WAVEfmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(data))+data
    return b'RIFF'+struct.pack('<I',len(body))+body
fmt=struct.pack('<HHIIHH',1,1,16000,32000,2,16)
valid=wav(fmt,b'\0'*16000)
invalid={
    'empty':b'',
    'truncated-header':b'RIFF\xff\xff\xff\xffWAVE',
    'truncated-data':valid[:-2],
    'missing-format':b'RIFF'+struct.pack('<I',4)+b'WAVE',
    'odd-pcm-length':wav(fmt,b'\0'*15999),
    'nan-float':wav(struct.pack('<HHIIHH',3,1,16000,64000,4,32),struct.pack('<f',float('nan'))*8000),
    'unsupported-a-law':wav(struct.pack('<HHIIHH',6,1,16000,16000,1,8),b'\0'*8000),
}
rows=[]
def case(name,args,expect=None):
    report=folder/(name+'.json')
    if report.exists(): report.unlink()
    run=subprocess.run([str(exe),'--report',str(report),*map(str,args)],capture_output=True,text=True,encoding='utf-8',timeout=60)
    body=json.loads(report.read_text('utf-8')) if report.exists() else {}
    error=str(body.get('error',''))
    rows.append({'case':name,'exit_code':run.returncode,'report':body,'expected_error_fragment':expect,
                 'success':run.returncode!=0 and body.get('success') is False and bool(error) and (expect is None or expect.lower() in error.lower())})
for name,data in invalid.items():
    path=folder/(name+'.wav')
    path.write_bytes(data)
    case(name,['--preprocess',path,'--model',model])
case('missing-model',['--inspect','--model',folder/'missing'])
if not DIRECTML_VALIDATED:
    case('unvalidated-directml',['--provider','directml'],expect='not validated')
    case('directml-no-adapter',['--inspect','--model',model,'--provider','directml','--experimental-directml'],expect='explicit --adapter')
case('unknown-provider',['--inspect','--model',model,'--provider','cuda'],expect='provider')
# Adapter validation: the software renderer index comes from --adapters (a successful, non-failure run).
adapters=subprocess.run([str(exe),'--adapters','--model',str(model),'--report',str(folder/'adapters.json')],capture_output=True,text=True,encoding='utf-8',timeout=60)
listing=json.loads((folder/'adapters.json').read_text('utf-8')) if adapters.returncode==0 else {'adapters':[]}
case('directml-negative-adapter',['--inspect','--model',model,'--provider','directml',*experimental,'--adapter','-1'],expect='negative')
case('directml-bad-adapter',['--inspect','--model',model,'--provider','directml',*experimental,'--adapter','99'],expect='does not exist')
case('directml-bad-adapter-word',['--inspect','--model',model,'--provider','directml',*experimental,'--adapter','fast'],expect='invalid --adapter')
software=[a['directml_index'] for a in listing['adapters'] if a.get('software')]
if software: case('directml-software-adapter',['--inspect','--model',model,'--provider','directml',*experimental,'--adapter',str(software[0])],expect='software renderer')
else: rows.append({'case':'directml-software-adapter','skipped':'no software adapter enumerated','success':True})
low=[a['directml_index'] for a in listing['adapters'] if not a.get('software') and a.get('duplicate_of') is None and not a.get('eligible') and 'memory' in a.get('reason','')]
if low: case('directml-low-memory-adapter',['--inspect','--model',model,'--provider','directml',*experimental,'--adapter',str(low[0])],expect='dedicated video memory')
else: rows.append({'case':'directml-low-memory-adapter','skipped':'no low-memory hardware adapter enumerated','success':True})
case('large-model-cpu-interface',['--large-model','--provider','cpu'],expect='DirectML only')
case('invalid-threads',['--threads','0'])
case('invalid-token-limit',['--max-tokens','0'])
case('unknown-option',['--does-not-exist'])
case('nan-duration',['--capture','--duration','nan'])
case('negative-pace',['--benchmark',ROOT/'regression/silence.wav','--simulate-live','--pace','-1'])
case('infinite-tail',['--capture','--tail-silence','inf'])
case('invalid-provisional-tokens',['--capture','--provisional-max-tokens','0'])
case('missing-package-no-development-fallback',['--inspect'])
result={'success':all(x['success'] for x in rows),'directml_validated':DIRECTML_VALIDATED,'adapters_listed':len(listing['adapters']),'cases':rows}
(ROOT/'reports/cli-failures.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='cases'},indent=2))
for row in rows: print(row['case'],'OK' if row['success'] else 'FAIL',row.get('skipped',''),str(row.get('report',{}).get('error',''))[:120])
raise SystemExit(0 if result['success'] else 1)
