"""Bounded native DirectML probe. Requires a passing CPU regression first."""
import json
import subprocess
import time
from bootstrap import ROOT

folder=ROOT/'reports/resume-audit'
cpu=json.loads((folder/'verification-1.7b-cpu.json').read_text('utf-8'))
if not cpu.get('success') or len(cpu.get('fixtures',[]))!=24: raise SystemExit('Complete CPU regression must pass before DirectML')
exe=ROOT/'build/Release/AsrWin.exe'
report=folder/'directml-1.7b.json'
profile=folder/'directml-profile'
cmd=[str(exe),'--benchmark',str(ROOT/'regression/english-0002.wav'),'--provider','directml','--experimental-directml','--adapter','0',
     '--model',str(ROOT/'models/qwen3-asr-1.7b-fp32'),'--threads','6','--report',str(report),'--profile',str(profile)]
start=time.perf_counter()
with (folder/'directml-1.7b.log').open('w',encoding='utf-8') as log:
    process=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
    try: code=process.wait(timeout=240);timed_out=False
    except subprocess.TimeoutExpired:
        process.kill();process.wait();code=process.returncode;timed_out=True
result={'command':cmd,'exit_code':code,'timed_out':timed_out,'wall_seconds':time.perf_counter()-start,
        'adapter':json.loads((folder/'diagnostics.json').read_text('utf-8'))['graphics_adapters'][0],
        'cpu_regression_pass':True,'enabled_for_live':False}
if report.exists(): result['benchmark']=json.loads(report.read_text('utf-8'))
placement={}
for path in profile.glob('*.json'):
    events=json.loads(path.read_text('utf-8')); providers={};copies=[]
    for event in events:
        provider=event.get('args',{}).get('provider')
        if provider:
            bucket=providers.setdefault(provider,{'calls':0,'total_us':0,'nodes':set()})
            bucket['calls']+=1;bucket['total_us']+=event.get('dur',0);bucket['nodes'].add(event.get('name',''))
        if any(x in event.get('name','').lower() for x in ('memcpy','copyfrom','copyto')): copies.append(event)
    for bucket in providers.values(): bucket['unique_nodes']=len(bucket.pop('nodes'))
    placement[path.name]={'providers':providers,'copy_events':len(copies),'copy_duration_us':sum(x.get('dur',0) for x in copies)}
result['profile_placement']=placement
result['success']=code==0 and not timed_out and result.get('benchmark',{}).get('success',False)
if not result['success']: result['limitation']='Graph loading or execution did not complete successfully; no valid end-to-end speedup or transfer-cost claim can be made.'
(folder/'directml-probe.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ('benchmark','command')},indent=2))
raise SystemExit(0 if result['success'] else 2)
