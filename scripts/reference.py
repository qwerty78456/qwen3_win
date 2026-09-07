"""Independent proof oracle using the unmodified official Qwen processor/model.

This script and its Python dependencies are build-time tools, never app runtime
dependencies. It does not import any ONNX exporter wrapper or ONNX runtime.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
os.environ['HF_HUB_OFFLINE'] = '1'
os.environ['TRANSFORMERS_OFFLINE'] = '1'
os.environ['TOKENIZERS_PARALLELISM'] = 'false'
CORE=next((ROOT/'.deps/qwen-reference').glob('*/qwen_asr/core'))
sys.path.insert(0, str(CORE))
import numpy as np
import soundfile as sf
try:
    import torch
except ModuleNotFoundError:
    torch=None
# Import the official processor file directly; its package __init__ also imports
# the heavyweight model. The file itself is unmodified and has no relative imports.
spec=importlib.util.spec_from_file_location('official_qwen_processor',CORE/'transformers_backend/processing_qwen3_asr.py')
processor_module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(processor_module)
Qwen3ASRProcessor=processor_module.Qwen3ASRProcessor
from bootstrap import sha256
from shipped import MODEL_DIR, REFERENCE_DIR, remote

def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

def floats(path, value):
    if torch is not None and isinstance(value, torch.Tensor): value = value.detach().cpu().float().numpy()
    np.asarray(value, dtype='<f4').tofile(path)

def prepare(processor):
    out = MODEL_DIR
    filters = processor.feature_extractor.mel_filters.T.astype('<f4')
    if filters.shape != (128,201): raise RuntimeError(f'Unexpected mel filter shape: {filters.shape}')
    filters.tofile(out/'mel_filters.bin')
    messages=[{'role':'system','content':''},{'role':'user','content':[{'type':'audio','audio':''}]}]
    prompt=processor.apply_chat_template(messages, add_generation_prompt=True, tokenize=False)
    prefix,suffix=prompt.split('<|audio_pad|>')
    cases=[]
    for text in [prefix,suffix,prompt,'你好，今天开会。','Hello, world!','请打开 the presentation，然后继续。','<|endoftext|>','<|im_end|>']:
        cases.append({'text':text,'ids':processor.tokenizer.encode(text,add_special_tokens=False)})
    write_json(out/'prompt_reference.json',{'prefix':prefix,'suffix':suffix,'cases':cases,
        'source':'Official Qwen3ASRProcessor.apply_chat_template and Qwen2TokenizerFast; empty system context, automatic language'})
    print('Official prompt:', repr(prompt), flush=True)
    print('Prefix:', cases[0]['ids'], 'Suffix:', cases[1]['ids'], flush=True)
    return prompt

def load_audio(path):
    audio, rate=sf.read(path, dtype='float32', always_2d=True)
    audio=audio.mean(axis=1,dtype=np.float32)
    if rate != 16000:
        import librosa
        audio=librosa.resample(audio,orig_sr=rate,target_sr=16000).astype(np.float32)
    peak=np.max(np.abs(audio))
    if peak>1: audio=audio/peak
    # Application minimum length: 1.0 s (one full 100-frame encoder conv chunk); see src/mel.cpp.
    audio=np.pad(audio,(0,max(0,16000-len(audio))))
    return audio

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--fixture', action='append')
    parser.add_argument('--threads',type=int,default=6)
    parser.add_argument('--max-tokens',type=int,default=512)
    parser.add_argument('--wait-assets',action='store_true')
    parser.add_argument('--global-attention',action='store_true',help='Reproduce the pinned eager path that omits the encoder window mask')
    parser.add_argument('--manifest',default='regression/manifest.json',help='fixture manifest (regression or evaluation set)')
    parser.add_argument('--extra',action='append',default=[],help='Ad-hoc WAV (boundary tests); id is the file stem, output under traces/reference and regression/reference-extra')
    args=parser.parse_args()
    if torch is not None:
        torch.set_num_threads(args.threads)
        torch.set_num_interop_threads(1)
    processor=Qwen3ASRProcessor.from_pretrained(REFERENCE_DIR,local_files_only=True)
    prompt=prepare(processor)
    if args.prepare_only: return
    if torch is None: raise RuntimeError('Install the pinned reference Torch wheel before generation')
    from transformers_backend import Qwen3ASRForConditionalGeneration
    assets=remote('reference_lock_key')['files']
    assets=[x for x in assets if x['path'].endswith('.safetensors')]
    if args.wait_assets and any(not (REFERENCE_DIR/x['path']).exists() for x in assets):
        print('Waiting for the pinned original-model download to finish...',flush=True)
        while any(not (REFERENCE_DIR/x['path']).exists() for x in assets): time.sleep(5)
    for asset in assets:
        file=REFERENCE_DIR/asset['path']
        if not file.is_file() or file.stat().st_size!=asset['bytes'] or sha256(file)!=asset['sha256']:
            raise RuntimeError('Original model failed its publisher SHA-256 check: '+asset['path'])
    manifest_path=ROOT/args.manifest
    manifest=json.loads(manifest_path.read_text('utf-8'))
    set_dir=manifest_path.parent
    if len(manifest['fixtures'])!=24:
        raise RuntimeError('Freeze all 24 fixtures before evaluation')
    chosen=[x for x in manifest['fixtures'] if not args.fixture or x['id'] in args.fixture]
    if args.fixture and len(chosen)!=len(args.fixture): raise RuntimeError('Unknown fixture ID')
    if args.extra:
        chosen=[{'id':Path(x).stem,'path':str(Path(x).resolve()),'sha256':sha256(Path(x)),'extra':True} for x in args.extra]
    print('Loading original Qwen3-ASR-1.7B FP32 on CPU...',flush=True)
    start=time.perf_counter()
    model=Qwen3ASRForConditionalGeneration.from_pretrained(REFERENCE_DIR,torch_dtype=torch.float32,
        attn_implementation='eager',local_files_only=True).eval()
    load_time=time.perf_counter()-start
    # The pinned official transformers backend defines Qwen3ASRAudioEncoder._prepare_attention_mask
    # (block-diagonal windows of n_window_infer frames, the behaviour of its flash-attention path and of
    # the transformers-integrated implementation) but its eager/SDPA path never applies it, so the
    # encoder attends globally. The unmodified helper is applied here through the layer call so the
    # oracle reflects the designed windowed inference. --global-attention reproduces the unmasked path.
    encoder_attention='global (pinned eager path, no window mask)'
    tower=model.thinker.audio_tower
    Layer=type(tower.layers[0]); original_forward=Layer.forward
    if not args.global_attention:
        def windowed_forward(self,hidden_states,cu_seqlens,attention_mask=None,**kw):
            if attention_mask is None: attention_mask=tower._prepare_attention_mask(hidden_states,cu_seqlens)
            return original_forward(self,hidden_states,cu_seqlens,attention_mask=attention_mask,**kw)
        Layer.forward=windowed_forward
        encoder_attention='windowed (official _prepare_attention_mask applied, n_window_infer=%d)'%tower.n_window_infer
    print('Encoder attention:',encoder_attention,flush=True)
    for fixture in chosen:
        print('Reference:',fixture['id'],flush=True)
        wav=Path(fixture['path']) if fixture.get('extra') else set_dir/fixture['path']
        if sha256(wav)!=fixture['sha256']: raise RuntimeError('Fixture hash mismatch')
        audio=load_audio(wav)
        folder=ROOT/'traces/reference'/fixture['id']
        folder.mkdir(parents=True,exist_ok=True)
        floats(folder/'pcm.f32',audio)
        inputs=processor(text=[prompt],audio=[audio],return_tensors='pt',padding=True)
        floats(folder/'mel.f32',inputs['input_features'])
        ids=inputs['input_ids'][0].tolist()
        token_count=ids.count(processor.tokenizer.convert_tokens_to_ids('<|audio_pad|>'))
        write_json(folder/'prompt.json',{'ids':ids,'positions':list(range(len(ids))),
            'audio_offset':ids.index(processor.tokenizer.convert_tokens_to_ids('<|audio_pad|>')),
            'mel_frames':inputs['input_features'].shape[-1],'audio_tokens':token_count})
        step=[0]
        cache_shapes=[]
        def audio_hook(module, args, output): floats(folder/'encoder.f32',output.last_hidden_state)
        def decoder_pre(module, args, kwargs):
            if step[0]==0:
                floats(folder/'input_embeds.f32',kwargs['inputs_embeds'])
                write_json(folder/'position_ids.json',kwargs['position_ids'].tolist())
        def generation_hook(module,args,output):
            n=step[0]
            cache=output.past_key_values
            keys=torch.stack([cache[i][0] for i in range(len(cache))])
            values=torch.stack([cache[i][1] for i in range(len(cache))])
            cache_shapes.append({'step':n,'keys':list(keys.shape),'values':list(values.shape),
                'next_token':int(output.logits[0,-1].argmax())})
            if n in (0,1,7):
                floats(folder/f'logits_{n}.f32',output.logits[0,-1])
                floats(folder/f'keys_{n}.f32',keys)
                floats(folder/f'values_{n}.f32',values)
            step[0]+=1
        hooks=[model.thinker.audio_tower.register_forward_hook(audio_hook),
            model.thinker.model.register_forward_pre_hook(decoder_pre,with_kwargs=True),
            model.thinker.register_forward_hook(generation_hook)]
        start=time.perf_counter()
        try:
            with torch.inference_mode():
                generated=model.generate(**inputs,max_new_tokens=args.max_tokens,do_sample=False,use_cache=True)
        finally:
            for hook in hooks: hook.remove()
        elapsed=time.perf_counter()-start
        tokens=generated.sequences[0,len(ids):].tolist()
        raw=processor.tokenizer.decode(tokens,skip_special_tokens=False,clean_up_tokenization_spaces=False)
        completion='eos' if tokens and tokens[-1] in (151643,151645) else 'token_limit'
        if '<asr_text>' in raw: text=raw.split('<asr_text>',1)[1]
        elif raw in ('<|im_end|>','<|endoftext|>'): text=''
        else: raise RuntimeError('Official output has no ASR delimiter: '+raw)
        text=text.replace('<|im_end|>','').replace('<|endoftext|>','')
        result={'text':text,'raw_output':raw,'tokens':tokens,'completion':completion,
            'prompt_ids':ids,'cache_steps':cache_shapes,'elapsed_seconds':elapsed,'model_load_seconds':load_time,
            'audio_seconds':len(audio)/16000,'dtype':'float32','attention':'eager','encoder_attention':encoder_attention,'threads':args.threads,
            'reference_source_revision':json.loads((ROOT/'dependencies.lock.json').read_text())['sources']['qwen-reference']['revision'],
            'reference_model_repo':remote('reference_lock_key')['repo'],'reference_model_revision':remote('reference_lock_key')['revision'],
            'torch_version':torch.__version__}
        write_json(folder/'result.json',result)
        if fixture.get('extra'):
            write_json(ROOT/'regression/reference-extra'/(fixture['id']+'.json'),result)
        else:
            refpath=set_dir/'reference'/(fixture['id']+'.json')
            write_json(refpath,result)
            fixture.update(reference_path=refpath.relative_to(set_dir).as_posix(),reference_sha256=sha256(refpath))
            manifest['reference_status']='complete' if all('reference_sha256' in x for x in manifest['fixtures']) else 'pending'
            write_json(manifest_path,manifest)
        print(f'{elapsed:.2f}s {completion}: {text}',flush=True)

if __name__=='__main__': main()
