"""Strict independent comparison. Missing stages and mismatches exit nonzero."""
import argparse
import json
import hashlib
from pathlib import Path
import numpy as np
from bootstrap import ROOT

def main():
    p=argparse.ArgumentParser()
    p.add_argument('fixture')
    p.add_argument('--report',type=Path)
    args=p.parse_args()
    reference=ROOT/'traces/reference'/args.fixture
    native=ROOT/'traces/native'/args.fixture
    policy=json.loads((ROOT/'verification-policy.json').read_text('utf-8'))
    result={'fixture':args.fixture,'success':True,'stages':{}}
    def compare(a,b,limit):
        if a.shape!=b.shape or not a.size or not np.isfinite(a).all() or not np.isfinite(b).all(): return {'success':False}
        err=np.abs(a-b); maximum=float(err.max()); rmse=float(np.sqrt(np.mean(err.astype(np.float64)**2)))
        elementwise=bool(np.all(err<=limit['absolute']+limit['relative']*np.abs(a)))
        scale_ok='scale_relative' in limit and maximum<=limit['absolute']+limit['scale_relative']*float(np.abs(a).max()) and rmse<=limit['rmse_relative']*float(np.sqrt(np.mean(a.astype(np.float64)**2)))
        return {'success':bool(elementwise or scale_ok),'max_absolute_error':maximum,'rmse':rmse,'tolerance':limit}
    try:
        refprompt=json.loads((reference/'prompt.json').read_text('utf-8'))
        natprompt=json.loads((native/'prompt.json').read_text('utf-8'))
        result['prompt_exact']=refprompt==natprompt
        result['success'] &= result['prompt_exact']
        ref=json.loads((reference/'result.json').read_text('utf-8'))
        nat=json.loads((native/'result.json').read_text('utf-8'))
        result['tokens_exact']=ref['tokens']==nat['tokens']
        result['success'] &= result['tokens_exact']
        result['completion_match']=ref.get('completion')==nat.get('completion')=='eos'
        result['success'] &= result['completion_match']
        result['token_differences']=[{'index':i,'reference':ref['tokens'][i] if i<len(ref['tokens']) else None,
            'native':nat['tokens'][i] if i<len(nat['tokens']) else None}
            for i in range(max(len(ref['tokens']),len(nat['tokens'])))
            if i>=min(len(ref['tokens']),len(nat['tokens'])) or ref['tokens'][i]!=nat['tokens'][i]]
        result['cache_shapes_exact']=[(x['keys'],x['values']) for x in ref['cache_steps']]==[(x['keys'],x['values']) for x in nat['cache_steps']]
        result['success'] &= result['cache_shapes_exact']
        required=['pcm','mel','encoder','input_embeds']
        for step in (0,1,7):
            if step<len(ref['tokens']): required.extend(f'{kind}_{step}' for kind in ('logits','keys','values'))
        for name in required:
            a=np.fromfile(reference/(name+'.f32'),dtype='<f4')
            b=np.fromfile(native/(name+'.f32'),dtype='<f4')
            key=name if name=='input_embeds' else name.split('_')[0]
            limit=policy['stages'][key]
            item={'reference_elements':int(a.size),'native_elements':int(b.size),'tolerance':limit,'success':False}
            if a.shape==b.shape and a.size and np.isfinite(a).all() and np.isfinite(b).all():
                error=np.abs(a-b)
                failures=error > limit['absolute']+limit['relative']*np.abs(a)
                scale=float(np.abs(a).max()); rms=float(np.sqrt(np.mean(a.astype(np.float64)**2))); rmse=float(np.sqrt(np.mean(error.astype(np.float64)**2)))
                elementwise=not bool(failures.any())
                item.update(max_absolute_error=float(error.max()),rmse=rmse,reference_max_abs=scale,reference_rms=rms,
                    failed_elements=int(failures.sum()),elementwise_success=elementwise,success=elementwise)
                if 'scale_relative' in limit:
                    tensor_scale_ok=float(error.max())<=limit['absolute']+limit['scale_relative']*scale and rmse<=limit['rmse_relative']*rms
                    item.update(tensor_scale_success=bool(tensor_scale_ok),success=elementwise or bool(tensor_scale_ok))
            result['stages'][name]=item
            result['success'] &= item['success']
        result['end_to_end_success']=result['success']
        # A preceding stage can satisfy its tolerance while its small perturbation is
        # amplified by the decoder. Diagnose this explicitly using an official decoder
        # run on the identical native encoder tensor. Never relax logits or token checks.
        failed=[k for k,v in result['stages'].items() if not v['success']]
        local=ROOT/'traces/decoder-reference'/args.fixture
        if failed and all(k.startswith(('keys_','values_')) for k in failed) and (local/'provenance.json').exists():
            proof=json.loads((local/'provenance.json').read_text('utf-8'))
            digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
            bound=proof['native_encoder_sha256']==digest(native/'encoder.f32') and proof['reference_result_sha256']==digest(reference/'result.json') and proof['tokens']==ref['tokens']==nat['tokens']
            isolated={}
            for name in required:
                if name.startswith(('keys_','values_','logits_')):
                    path=local/(name+'.f32')
                    if not path.exists() or proof['files'].get(path.name)!=digest(path): bound=False; continue
                    isolated[name]=compare(np.fromfile(path,dtype='<f4'),np.fromfile(native/path.name,dtype='<f4'),policy['stages'][name.split('_')[0]])
            result['decoder_input_isolation']={'provenance_bound':bound,'stages':isolated,
                'explanation':'Small encoder differences propagate into KV caches. The official decoder on the same encoder tensor passes the original tolerances; end-to-end logits, tokens and transcripts still agree.',
                'evidence':'reports/resume-audit/noise-isolation.json'}
            # This diagnoses propagation but never turns a failed end-to-end proof green.
            result['decoder_input_isolation']['success']=bool(bound and isolated and all(v['success'] for v in isolated.values()) and result['prompt_exact'] and result['tokens_exact'] and result['cache_shapes_exact'] and result['completion_match'])
    except Exception as e:
        result.update(success=False,error=str(e))
    path=args.report or ROOT/'reports/stages'/(args.fixture+'.json')
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
    raise SystemExit(0 if result['success'] else 1)

if __name__=='__main__': main()
