"""Exercise invalid input and missing proof assets through the shipped executable."""
import json
from pathlib import Path
import struct
import subprocess
from bootstrap import ROOT

folder=ROOT/'traces/cli-failures'
folder.mkdir(parents=True,exist_ok=True)
exe=ROOT/'build/Release/AsrWin.exe'
from shipped import MODEL_DIR
model=MODEL_DIR
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
def case(name,args):
    report=folder/(name+'.json')
    if report.exists(): report.unlink()
    run=subprocess.run([str(exe),'--report',str(report),*map(str,args)],capture_output=True,text=True,encoding='utf-8',timeout=30)
    body=json.loads(report.read_text('utf-8')) if report.exists() else {}
    rows.append({'case':name,'exit_code':run.returncode,'report':body,
                 'success':run.returncode!=0 and body.get('success') is False and bool(body.get('error'))})
for name,data in invalid.items():
    path=folder/(name+'.wav')
    path.write_bytes(data)
    case(name,['--preprocess',path,'--model',model])
case('missing-model',['--inspect','--model',folder/'missing'])
case('unvalidated-directml',['--provider','directml'])
case('invalid-threads',['--threads','0'])
case('invalid-token-limit',['--max-tokens','0'])
case('unknown-option',['--does-not-exist'])
case('nan-duration',['--capture','--duration','nan'])
case('negative-pace',['--benchmark',ROOT/'regression/silence.wav','--simulate-live','--pace','-1'])
case('infinite-tail',['--capture','--tail-silence','inf'])
case('invalid-provisional-tokens',['--capture','--provisional-max-tokens','0'])
case('directml-no-adapter',['--inspect','--provider','directml','--experimental-directml'])
case('missing-package-no-development-fallback',['--inspect'])
result={'success':all(x['success'] for x in rows),'cases':rows}
(ROOT/'reports/cli-failures.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['success'] else 1)
