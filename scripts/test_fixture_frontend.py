"""Check real regression audio through native frontend before generation."""
import json
import subprocess
import numpy as np
from reference import ROOT,Qwen3ASRProcessor,prepare,load_audio
from shipped import MODEL_DIR, REFERENCE_DIR

processor=Qwen3ASRProcessor.from_pretrained(REFERENCE_DIR,local_files_only=True)
prompt=prepare(processor)
manifest=json.loads((ROOT/'regression/manifest.json').read_text('utf-8'))
results=[]
for item in manifest['fixtures']:
    folder=ROOT/'traces/frontend'/item['id']
    folder.mkdir(parents=True,exist_ok=True)
    wav=ROOT/'regression'/item['path']
    args=[str(ROOT/'build/Release/AsrWin.exe'),'--model',str(MODEL_DIR)]
    subprocess.run([*args,'--preprocess',str(wav),'--trace',str(folder),'--report',str(folder/'preprocess.json')],check=True,capture_output=True)
    subprocess.run([*args,'--segment',str(wav),'--report',str(folder/'segments.json')],check=True,capture_output=True)
    samples=load_audio(wav)
    reference=processor(text=[prompt],audio=[samples],return_tensors='np',padding=True)
    pcm=np.fromfile(folder/'pcm.f32',dtype='<f4')
    mel=np.fromfile(folder/'mel.f32',dtype='<f4')
    expected=reference['input_features'].reshape(-1)
    report=json.loads((folder/'preprocess.json').read_text('utf-8'))
    segments=json.loads((folder/'segments.json').read_text('utf-8'))['segments']
    row={'id':item['id'],'pcm_exact':bool(np.array_equal(samples,pcm)),
         'mel_shape_exact':expected.shape==mel.shape,'segments':segments}
    if expected.shape==mel.shape:
        error=np.abs(expected-mel)
        row['mel_max_error']=float(error.max())
        row['mel_within_tolerance']=bool(np.all(error<=0.0001+0.0001*np.abs(expected)))
    row['audio_tokens_exact']=int((reference['input_ids']==151676).sum())==report['audio_tokens']
    row['speech_gate_expected']=bool(segments) if item['category']!='non-speech' else not segments
    row['bounded_segments']=all(s['samples']<=240000 for s in segments)
    row['nonoverlapping_segments']=all(a['end_sample']<=b['start_sample'] for a,b in zip(segments,segments[1:]))
    row['success']=all(row.get(k,False) for k in ['pcm_exact','mel_shape_exact','mel_within_tolerance',
        'audio_tokens_exact','speech_gate_expected','bounded_segments','nonoverlapping_segments'])
    print(item['id'],'PASS' if row['success'] else 'FAIL',flush=True)
    results.append(row)
report={'success':all(x['success'] for x in results),'fixtures':results}
(ROOT/'reports/fixture-frontend.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
raise SystemExit(0 if report['success'] else 1)
