"""Freeze the exact baseline before inference tests; fail on incomplete assets.

`--model-key large` freezes the optional GPU-only model named in shipped-model.json (directml.large_model).
"""
import argparse
import json
from bootstrap import ROOT, sha256
from shipped import model_config, remote
parser=argparse.ArgumentParser()
parser.add_argument('--model-key',default='default',choices=['default','large'])
parser.add_argument('--variant',default=None,choices=['fp32','int4'])
args=parser.parse_args()
cfg=model_config(args.model_key)
variant=args.variant or cfg['variant']
lock=json.loads((ROOT/'dependencies.lock.json').read_text('utf-8'))
source=remote('onnx_lock_key',args.model_key); reference=remote('reference_lock_key',args.model_key)
pinned_files={a['path']:a for a in source['files']}
model=ROOT/cfg['model_dir']
files=[]
decoders={'fp32':['decoder_init.onnx','decoder_step.onnx','decoder_weights.data'],'int4':['decoder_init.int4.onnx','decoder_step.int4.onnx','decoder_weights.int4.data']}[variant]
for name in ['config.json','tokenizer.json','prompt_reference.json','mel_filters.bin','encoder.onnx',*decoders,'embed_tokens.bin']:
    path=model/name
    if not path.is_file(): raise RuntimeError('Missing model asset: '+name)
    record={'path':name,'bytes':path.stat().st_size,'sha256':sha256(path)}
    pinned=lock['files'].get(path.relative_to(ROOT).as_posix()) or pinned_files.get(name)
    if pinned and any(record[k]!=pinned[k] for k in ('bytes','sha256') if pinned.get(k) is not None):  # non-LFS publisher entries carry no hash
        raise RuntimeError('Asset does not match pinned acquisition/publisher hash: '+name)
    files.append(record)
configuration=cfg['configuration'] if variant==cfg['variant'] else cfg['configuration']+' ('+variant+' decoders)'
manifest={'schema':1,'configuration':configuration,'variant':variant,
          'source_model':{'repo':source['repo'],'revision':source['revision']},'reference_model':{'repo':reference['repo'],'revision':reference['revision']},
          'runtime':lock['onnxruntime'],'files':files}
(model/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Model manifest frozen:',cfg['model_dir'],sum(x['bytes'] for x in files),'bytes')
