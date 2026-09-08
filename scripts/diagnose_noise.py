"""Isolate frontend sensitivity using the official model; never changes the oracle or tolerances."""
import json
import hashlib
import numpy as np
import torch
from reference import ROOT, Qwen3ASRProcessor, load_audio
from transformers_backend import Qwen3ASRForConditionalGeneration

torch.set_num_threads(4)
checkpoint=ROOT/'reference-model-0.6b'
processor=Qwen3ASRProcessor.from_pretrained(checkpoint,local_files_only=True)
model=Qwen3ASRForConditionalGeneration.from_pretrained(checkpoint,torch_dtype=torch.float32,
    attn_implementation='eager',local_files_only=True).eval()
folder=ROOT/'traces/reference/low-noise'
native=ROOT/'traces/native/low-noise'
prompt=processor.apply_chat_template([{'role':'system','content':''},{'role':'user','content':[{'type':'audio','audio':''}]}],add_generation_prompt=True,tokenize=False)
inputs=processor(text=[prompt],audio=[load_audio(ROOT/'regression/low-noise.wav')],return_tensors='pt',padding=True)
results=[]
for mode in ['official','native_mel','native_encoder']:
    values={k:v.clone() for k,v in inputs.items()}
    if mode=='native_mel': values['input_features']=torch.from_numpy(np.fromfile(native/'mel.f32',dtype='<f4').reshape(values['input_features'].shape))
    traces={}
    local=ROOT/'traces/decoder-reference/low-noise'
    local.mkdir(parents=True,exist_ok=True)
    def encoder_hook(module,args,output):
        if mode=='native_encoder': output.last_hidden_state=torch.from_numpy(np.fromfile(native/'encoder.f32',dtype='<f4').reshape(output.last_hidden_state.shape))
        traces['encoder']=output.last_hidden_state.detach().numpy().flatten()
    def output_hook(module,args,output):
        if mode=='native_encoder':
            step=len(local_steps)
            local_steps.append(int(output.logits[0,-1].argmax()))
            if step in (0,1,7):
                output.logits[0,-1].detach().numpy().astype('<f4').tofile(local/f'logits_{step}.f32')
                for name,index in [('keys',0),('values',1)]:
                    torch.stack([output.past_key_values[i][index] for i in range(len(output.past_key_values))]).detach().numpy().astype('<f4').tofile(local/f'{name}_{step}.f32')
        if 'logits_0' in traces: return
        traces['logits_0']=output.logits[0,-1].detach().numpy().flatten()
        for name,index in [('keys_0',0),('values_0',1)]:
            traces[name]=torch.stack([output.past_key_values[i][index] for i in range(len(output.past_key_values))]).detach().numpy().flatten()
    local_steps=[]
    hooks=[model.thinker.audio_tower.register_forward_hook(encoder_hook),model.thinker.register_forward_hook(output_hook)]
    with torch.inference_mode(): generated=model.generate(**values,max_new_tokens=32,do_sample=False,use_cache=True)
    for hook in hooks: hook.remove()
    row={'mode':mode,'tokens':generated.sequences[0,values['input_ids'].shape[-1]:].tolist(),'stages':{}}
    for name,arr in traces.items():
        row['stages'][name]={}
        for label,path in [('original_reference',folder),('native',native)]:
            other=np.fromfile(path/(name+'.f32'),dtype='<f4')
            diff=arr-other
            row['stages'][name][label]={'max_abs':float(np.abs(diff).max()),'rmse':float(np.sqrt(np.mean(diff.astype(np.float64)**2)))}
    results.append(row)
    if mode=='native_encoder':
        def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
        evidence={'scope':'Official decoder with the native encoder output as input; end-to-end oracle remains unchanged',
            'native_encoder_sha256':digest(native/'encoder.f32'),'reference_result_sha256':digest(folder/'result.json'),
            'reference_model':'Qwen/Qwen3-ASR-0.6B','tokens':row['tokens'],
            'files':{p.name:digest(p) for p in local.glob('*.f32')}}
        (local/'provenance.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
result={'fixture':'low-noise','model':'Qwen/Qwen3-ASR-0.6B','threads':4,'comparisons':results}
(ROOT/'reports/resume-audit/noise-isolation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
