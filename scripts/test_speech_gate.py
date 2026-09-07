"""Raw-model silence is tested separately; this verifies application segmentation."""
import json
import math
import random
import struct
import subprocess
import wave
from bootstrap import ROOT

folder=ROOT/'traces/speech-gate'
folder.mkdir(parents=True,exist_ok=True)
results=[]
for i,(name,duration) in enumerate([('silence-short',0.5),('silence',2),('silence-long',15),('low-noise',3),('tone',2),('clicks',4)]):
    rng=random.Random(20260907+i)
    samples=[]
    for n in range(int(duration*16000)):
        value=0
        if name=='low-noise': value=rng.randint(-20,20)
        if name=='tone': value=int(1000*math.sin(2*math.pi*1000*n/16000))
        if name=='clicks' and n%8000<160: value=rng.randint(-1000,1000)
        samples.append(value)
    path=folder/(name+'.wav')
    with wave.open(str(path),'wb') as w:
        w.setparams((1,2,16000,0,'NONE','not compressed'))
        w.writeframes(struct.pack('<'+'h'*len(samples),*samples))
    report=folder/(name+'.json')
    run=subprocess.run([str(ROOT/'build/Release/AsrWin.exe'),'--segment',str(path),'--report',str(report)],capture_output=True)
    item=json.loads(report.read_text('utf-8')) if report.exists() else {}
    results.append({'case':name,'success':run.returncode==0 and item.get('segments')==[], 'observed':item})
result={'success':all(x['success'] for x in results),'cases':results}
(ROOT/'reports/speech-gate.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['success'] else 1)
