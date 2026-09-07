"""Freeze the exact baseline before inference tests; fail on incomplete assets."""
import argparse
import json
from bootstrap import ROOT, sha256
from shipped import CONFIG, MODEL_DIR, remote
parser=argparse.ArgumentParser(); parser.add_argument('--variant',default=CONFIG['variant'],choices=['fp32','int4']); args=parser.parse_args()
lock=json.loads((ROOT/'dependencies.lock.json').read_text('utf-8'))
source=remote('onnx_lock_key'); reference=remote('reference_lock_key')
remote={a['path']:a for a in source['files']}
model=MODEL_DIR
files=[]
decoders={'fp32':['decoder_init.onnx','decoder_step.onnx','decoder_weights.data'],'int4':['decoder_init.int4.onnx','decoder_step.int4.onnx','decoder_weights.int4.data']}[args.variant]
for name in ['config.json','tokenizer.json','prompt_reference.json','mel_filters.bin','encoder.onnx',*decoders,'embed_tokens.bin']:
    path=model/name
    if not path.is_file(): raise RuntimeError('Missing model asset: '+name)
    record={'path':name,'bytes':path.stat().st_size,'sha256':sha256(path)}
    pinned=lock['files'].get(path.relative_to(ROOT).as_posix()) or remote.get(name)
    if pinned and any(record[k]!=pinned[k] for k in ('bytes','sha256') if pinned.get(k) is not None):  # non-LFS publisher entries carry no hash
        raise RuntimeError('Asset does not match pinned acquisition/publisher hash: '+name)
    files.append(record)
configuration=CONFIG['configuration'] if args.variant==CONFIG['variant'] else CONFIG['configuration']+' ('+args.variant+' decoders)'
manifest={'schema':1,'configuration':configuration,'variant':args.variant,
          'source_model':{'repo':source['repo'],'revision':source['revision']},'reference_model':{'repo':reference['repo'],'revision':reference['revision']},
          'runtime':lock['onnxruntime'],'files':files}
(model/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Model manifest frozen:',sum(x['bytes'] for x in files),'bytes')
