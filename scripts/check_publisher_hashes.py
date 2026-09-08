"""Verify acquired LFS assets against hashes supplied by the pinned publishers."""
import json
from bootstrap import ROOT,sha256
from shipped import CONFIG, MODEL_DIR, REFERENCE_DIR
remote=json.loads((ROOT/'remote-assets.lock.json').read_text('utf-8'))
records=[]
seen=set()
for kind,folder in [('onnx',ROOT/'models/qwen3-asr-1.7b-fp32'),('reference',ROOT/'reference-model'),(CONFIG['onnx_lock_key'],MODEL_DIR),(CONFIG['reference_lock_key'],REFERENCE_DIR)]:
    for asset in remote[kind]['files']:
        file=folder/asset['path']
        if file in seen: continue
        seen.add(file)
        if file.exists():
            digest=sha256(file)
            publisher=asset.get('sha256')
            passed=file.stat().st_size==asset['bytes'] and (not publisher or digest==publisher)
            records.append({'path':file.relative_to(ROOT).as_posix(),'success':passed,'local_sha256':digest,
                'verification':'publisher LFS SHA-256 and size' if publisher else 'publisher size only; no publisher SHA-256 supplied',
                'publisher_sha256_verified':bool(publisher) and passed})
        elif asset.get('sha256') and (file.name in ('encoder.onnx','decoder_init.onnx','decoder_step.onnx','decoder_weights.data','embed_tokens.bin') or file.suffix=='.safetensors'):
            records.append({'path':file.relative_to(ROOT).as_posix(),'success':False,'error':'Required publisher asset missing'})
result={'acquired_files_pass':all(x['success'] for x in records),'files':records,
        'complete_baseline_acquired':(ROOT/'models/qwen3-asr-1.7b-fp32/decoder_weights.data').is_file(),'shipped_model_acquired':(MODEL_DIR/'decoder_weights.data').is_file()}
(ROOT/'reports/publisher-hashes.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['acquired_files_pass'] else 1)
