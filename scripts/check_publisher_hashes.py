"""Verify acquired LFS assets against hashes supplied by the pinned publishers."""
import json
from bootstrap import ROOT,sha256
from shipped import CONFIG, MODEL_DIR, REFERENCE_DIR
remote=json.loads((ROOT/'remote-assets.lock.json').read_text('utf-8'))
records=[]
for kind,folder in [('onnx',ROOT/'models/qwen3-asr-1.7b-fp32'),('reference',ROOT/'reference-model'),(CONFIG['onnx_lock_key'],MODEL_DIR),(CONFIG['reference_lock_key'],REFERENCE_DIR)]:
    for asset in remote[kind]['files']:
        file=folder/asset['path']
        if file.exists():
            passed=file.stat().st_size==asset['bytes'] and sha256(file)==asset['sha256']
            records.append({'path':file.relative_to(ROOT).as_posix(),'success':passed})
            if not passed: raise RuntimeError('Publisher hash mismatch: '+str(file))
result={'acquired_files_pass':all(x['success'] for x in records),'files':records,
        'complete_baseline_acquired':(ROOT/'models/qwen3-asr-1.7b-fp32/decoder_weights.data').is_file(),'shipped_model_acquired':(MODEL_DIR/'decoder_weights.data').is_file()}
(ROOT/'reports/publisher-hashes.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
