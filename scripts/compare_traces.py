"""Strict independent comparison. Missing stages and mismatches exit nonzero."""
import argparse
import json
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
    try:
        refprompt=json.loads((reference/'prompt.json').read_text('utf-8'))
        natprompt=json.loads((native/'prompt.json').read_text('utf-8'))
        result['prompt_exact']=refprompt==natprompt
        result['success'] &= result['prompt_exact']
        ref=json.loads((reference/'result.json').read_text('utf-8'))
        nat=json.loads((native/'result.json').read_text('utf-8'))
        result['tokens_exact']=ref['tokens']==nat['tokens']
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
    except Exception as e:
        result.update(success=False,error=str(e))
    path=args.report or ROOT/'reports/stages'/(args.fixture+'.json')
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
    raise SystemExit(0 if result['success'] else 1)

if __name__=='__main__': main()
