"""Collect notices for components compiled or shipped, including Rust crates."""
import json
import os
from pathlib import Path
import shutil
import subprocess
from bootstrap import ROOT

DEST=ROOT/'licenses'
DEST.mkdir(exist_ok=True)
def copy(source,destination):
    if not source.is_file(): raise RuntimeError('Missing license: '+str(source))
    destination.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source,destination)

lock=json.loads((ROOT/'dependencies.lock.json').read_text('utf-8'))
for name,info in lock['sources'].items():
    source=next(p for p in (ROOT/'.deps'/name).iterdir() if p.is_dir())
    names={'kissfft':['COPYING','LICENSES/BSD-3-Clause','LICENSES/Unlicense'],
           'json':['LICENSE.MIT']}.get(name,['LICENSE'])
    for filename in names: copy(source/filename,DEST/name/filename)
for filename in ['LICENSE','ThirdPartyNotices.txt']:
    copy(ROOT/'.deps/onnxruntime'/filename,DEST/'onnxruntime'/filename)
for filename in ['LICENSE.txt','LICENSE-CODE.txt','ThirdPartyNotices.txt']:
    copy(ROOT/'.deps/directml'/filename,DEST/'directml'/filename)

tokenizer=next((ROOT/'.deps/tokenizers-cpp').glob('*/rust/Cargo.toml'))
env=dict(os.environ,CARGO_HOME=str(ROOT/'.cache/cargo'))
metadata=json.loads(subprocess.check_output(['cargo','metadata','--locked','--offline','--format-version','1',
    '--filter-platform','x86_64-pc-windows-msvc','--manifest-path',str(tokenizer)],env=env))
index=[]
for package in metadata['packages']:
    source=Path(package['manifest_path']).parent
    label=f"{package['name']}-{package['version']}"
    files=[]
    for path in source.rglob('*'):
        if path.is_file() and path.name.upper().startswith(('LICENSE','LICENCE','NOTICE','COPYING','COPYRIGHT','UNLICENSE')):
            relative=path.relative_to(source)
            copy(path,DEST/'rust'/label/relative)
            files.append(relative.as_posix())
    index.append({'name':package['name'],'version':package['version'],'license':package['license'],
                  'repository':package['repository'],'files':files})
(DEST/'rust/index.json').write_text(json.dumps(index,indent=2)+'\n',encoding='utf-8')
print('Collected notices for',len(index),'Rust packages and all native dependencies.')
