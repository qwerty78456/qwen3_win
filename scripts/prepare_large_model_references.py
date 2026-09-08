"""Install the archived 1.7B official-model references next to the regression and held-out sets.

The 1.7B proof (reports/proof-1.7b/) keeps the oracle outputs of the 24 frozen regression fixtures and of the
held-out evaluation set. This script copies them to regression/reference-1.7b/ and evaluation/reference-1.7b/
and writes regression/manifest-1.7b.json and evaluation/manifest-1.7b.json whose reference paths point at them,
so `AsrWin.exe --verify regression\\manifest-1.7b.json --large-model ...` can check the large model.

The archived files predate the `reference_model_repo` field that `--verify` now requires; it is added here from
remote-assets.lock.json after checking that the archived `reference_model_revision` is the pinned revision, and
the annotation is recorded in the file (`annotated_from`). Re-running `scripts/reference.py --model-key large`
regenerates the references and their stage traces from the official model and overwrites these files.
Nothing is downloaded.
"""
import json
from bootstrap import ROOT, sha256
from shipped import model_config, remote

cfg=model_config('large'); suffix=cfg['suffix']
archive=ROOT/'reports/proof-1.7b'
lock=remote('reference_lock_key','large')

def install(source_manifest, source_dir, set_dir, target_dir, target_manifest, frozen_manifest):
    manifest=json.loads(source_manifest.read_text('utf-8'))
    frozen=json.loads(frozen_manifest.read_text('utf-8'))
    ids=[f['id'] for f in manifest['fixtures']]
    if ids!=[f['id'] for f in frozen['fixtures']]: raise RuntimeError(f'{source_manifest}: fixture list differs from {frozen_manifest}')
    target_dir.mkdir(parents=True,exist_ok=True)
    annotated=0
    for fixture, current in zip(manifest['fixtures'],frozen['fixtures']):
        if fixture['sha256']!=current['sha256'] or fixture['path']!=current['path']: raise RuntimeError('Fixture audio differs from the frozen set: '+fixture['id'])
        if sha256(set_dir/fixture['path'])!=fixture['sha256']: raise RuntimeError('Fixture hash mismatch: '+fixture['id'])
        name=fixture['id']+'.json'; source=source_dir/name
        if fixture.get('reference_sha256') and sha256(source)!=fixture['reference_sha256']: raise RuntimeError('Archived reference hash mismatch: '+name)
        reference=json.loads(source.read_text('utf-8'))
        if reference.get('reference_model_revision')!=lock['revision']: raise RuntimeError(f'{name}: archived reference revision {reference.get("reference_model_revision")} is not the pinned {lock["revision"]}')
        if reference.get('reference_model_repo') not in (None,lock['repo']): raise RuntimeError(f'{name}: reference model {reference.get("reference_model_repo")} is not {lock["repo"]}')
        if 'reference_model_repo' not in reference:
            reference['reference_model_repo']=lock['repo']; reference['annotated_from']=source.relative_to(ROOT).as_posix(); annotated+=1
        (target_dir/name).write_text(json.dumps(reference,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        fixture['reference_path']=f'{target_dir.name}/{name}'; fixture['reference_sha256']=sha256(target_dir/name)
    manifest['reference_status']='complete'
    manifest['note']=f'References of {lock["repo"]} restored from {archive.relative_to(ROOT).as_posix()} by scripts/prepare_large_model_references.py ({annotated} files annotated with reference_model_repo); scripts/reference.py --model-key large regenerates them.'
    target_manifest.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Installed',len(ids),'references into',target_dir.relative_to(ROOT).as_posix(),'->',target_manifest.relative_to(ROOT).as_posix(),f'({annotated} annotated)')

install(archive/'regression-manifest.json',archive/'regression-reference',ROOT/'regression',ROOT/'regression'/('reference'+suffix),ROOT/cfg['regression_manifest'],ROOT/'regression/manifest.json')
install(archive/'evaluation-manifest.json',archive/'evaluation-reference',ROOT/'evaluation',ROOT/cfg['evaluation_reference_dir'],ROOT/'evaluation'/('manifest'+suffix+'.json'),ROOT/'evaluation/manifest.json')
