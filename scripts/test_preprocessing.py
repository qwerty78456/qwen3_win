"""Native Windows preprocessing versus official processor on deterministic boundaries."""
import json
from pathlib import Path
import subprocess
import numpy as np
import soundfile as sf
from reference import ROOT, Qwen3ASRProcessor, prepare
from shipped import MODEL_DIR, REFERENCE_DIR

processor=Qwen3ASRProcessor.from_pretrained(REFERENCE_DIR,local_files_only=True)
prompt=prepare(processor)
exe=ROOT/'build/Release/AsrWin.exe'
work=ROOT/'traces/preprocessing'
work.mkdir(parents=True,exist_ok=True)
model=MODEL_DIR
cases=[]
def run_case(name,audio,rate=16000,subtype='FLOAT'):
    folder=work/name
    folder.mkdir(exist_ok=True)
    path=folder/'input.wav'
    sf.write(path,audio,rate,subtype=subtype)
    run=subprocess.run([str(exe),'--preprocess',str(path),'--model',str(model),'--trace',str(folder),
        '--report',str(folder/'native.json')],capture_output=True,text=True,encoding='utf-8')
    result={'name':name,'exit_code':run.returncode,'success':False}
    try:
        if run.returncode: raise RuntimeError(run.stderr)
        samples=np.fromfile(folder/'pcm.f32',dtype='<f4')
        decoded,_=sf.read(path,dtype='float32',always_2d=True)
        expected=decoded.mean(axis=1,dtype=np.float32)
        if rate==16000:
            expected=np.pad(expected,(0,max(0,16000-len(expected))))
            result['pcm_exact']=bool(np.array_equal(expected,samples))
        else:
            # Different high-quality resamplers are not expected to be sample-exact.
            import librosa
            resampled=librosa.resample(expected,orig_sr=rate,target_sr=16000)
            result['resampling_length_delta']=int(len(samples)-len(resampled))
            common=min(len(samples),len(resampled))
            result['resampling_interior_rmse']=float(np.sqrt(np.mean((samples[200:common-200]-resampled[200:common-200])**2)))
            result['pcm_exact']=abs(result['resampling_length_delta'])<=2 and result['resampling_interior_rmse']<0.002
            expected=samples # Verify spectral stages independently from the resampler comparison.
        features=processor(text=[prompt],audio=[expected],return_tensors='np',padding=True)
        a=features['input_features'].reshape(-1)
        b=np.fromfile(folder/'mel.f32',dtype='<f4')
        result['reference_mel_shape']=list(features['input_features'].shape)
        result['native']=json.loads((folder/'native.json').read_text('utf-8'))
        result['mel_shape_exact']=a.size==b.size
        if a.shape==b.shape:
            error=np.abs(a-b)
            result['mel_max_absolute_error']=float(error.max())
            result['mel_within_tolerance']=bool(np.all(error<=0.0001+0.0001*np.abs(a)))
        audio_pad=processor.tokenizer.convert_tokens_to_ids('<|audio_pad|>')
        result['audio_tokens_exact']=int((features['input_ids']==audio_pad).sum())==result['native']['audio_tokens']
        result['success']=all(result.get(k,False) for k in ('pcm_exact','mel_shape_exact','mel_within_tolerance','audio_tokens_exact'))
    except Exception as e: result['error']=str(e)
    cases.append(result)
    print(name, 'PASS' if result['success'] else 'FAIL',flush=True)

for size in [1,159,160,199,200,201,7999,8000,8011,15840,15999,16000,16001,16160,127840,128000,128160,239999,240000]:  # 16000 = application minimum
    n=np.arange(size,dtype=np.float32)
    wave=(0.13*np.sin(2*np.pi*439*n/16000)+0.07*np.sin(2*np.pi*1731*n/16000)).astype(np.float32)
    run_case(f'boundary-{size}',wave)
for subtype in ['PCM_U8','PCM_16','PCM_24','PCM_32','FLOAT']:
    n=np.arange(32000,dtype=np.float32)
    mono=(0.2*np.sin(2*np.pi*440*n/16000)).astype(np.float32)
    run_case('stereo-'+subtype,np.stack([mono,mono*0.5],axis=1),subtype=subtype)
for rate in [8000,22050,44100,48000,96000]:
    n=np.arange(rate*2,dtype=np.float32)
    run_case(f'resample-{rate}',(0.2*np.sin(2*np.pi*440*n/rate)).astype(np.float32),rate=rate)
report={'success':all(c['success'] for c in cases),'cases':cases}
(ROOT/'reports').mkdir(exist_ok=True)
(ROOT/'reports/preprocessing.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
raise SystemExit(0 if report['success'] else 1)
