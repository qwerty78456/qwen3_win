"""Assemble the extract-and-run folder and archive from the Release build.

Build-time only. Every file in the folder is listed with its SHA-256 in
package-manifest.json so `AsrWin.exe --check-package` can validate an extracted
copy on a clean machine. The archive size and extracted size are measured and
written to reports/package.json.
"""
import argparse
import hashlib
import json
import shutil
import time
import zipfile
import re
from pathlib import Path
from bootstrap import ROOT, sha256
from shipped import CONFIG

BUILD = ROOT / 'build/Release'
RUNTIME = ['AsrWin.exe', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll',
           'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
           'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll', 'concrt140.dll', 'vccorlib140.dll']
MODEL = CONFIG['model_dir']

def copy(src, dst):
    if not src.is_file():
        raise RuntimeError(f'Missing package input: {src}')
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', default='0.1.0-tc1')
    parser.add_argument('--no-archive', action='store_true')
    parser.add_argument('--no-docs-refresh', action='store_true', help='do not regenerate docs/ from the reports before hashing')
    parser.add_argument('--replace',action='store_true',help='replace this exact existing version inside dist')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,60}',args.version): raise ValueError('Invalid package version')
    name = f'AsrWin-{args.version}-win-x64'
    dist = ROOT / 'dist'
    folder = dist / name
    if folder.exists():
        if not args.replace: raise RuntimeError('Package version already exists; choose a new version or use --replace')
        if folder.resolve().parent != dist.resolve() or not folder.resolve().is_relative_to(ROOT.resolve()): raise RuntimeError('Unsafe package replacement path')
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    for file in RUNTIME:
        copy(BUILD / file, folder / file)
    model_manifest = json.loads((ROOT / MODEL / 'manifest.json').read_text('utf-8'))
    copy(ROOT / MODEL / 'manifest.json', folder / MODEL / 'manifest.json')
    for item in model_manifest['files']:
        copy(ROOT / MODEL / item['path'], folder / MODEL / item['path'])
    copy(ROOT / MODEL / 'README.md', folder / MODEL / 'MODEL-CARD.md')
    for rel in ['THIRD_PARTY_NOTICES.md', 'verification-policy.json', 'dependencies.lock.json', 'remote-assets.lock.json', 'native-dependency.lock.json', 'shipped-model.json',
                'docs/PACKAGE-README.md', 'docs/PROOF.md', 'docs/BENCHMARK.md', 'docs/COMPATIBILITY.md', 'docs/PACKAGING.md', 'docs/RESUME-AUDIT.md', 'docs/CLEAN-WINDOWS-TEST.md']:
        target = folder / ('README.md' if rel == 'docs/PACKAGE-README.md' else rel)
        copy(ROOT / rel, target)
    shutil.copytree(ROOT / 'licenses', folder / 'licenses')
    regression = folder / 'regression'
    regression.mkdir()
    for file in (ROOT / 'regression').iterdir():
        if file.is_file():
            copy(file, regression / file.name)
    shutil.copytree(ROOT / 'regression/reference', regression / 'reference')
    docs = {'README.md': 'docs/PACKAGE-README.md', 'docs/PROOF.md': 'docs/PROOF.md', 'docs/BENCHMARK.md': 'docs/BENCHMARK.md', 'docs/COMPATIBILITY.md': 'docs/COMPATIBILITY.md'}
    def inventory():
        files = []
        for path in sorted(folder.rglob('*')):
            if path.is_file() and path.name != 'package-manifest.json':
                files.append({'path': path.relative_to(folder).as_posix(), 'bytes': path.stat().st_size, 'sha256': sha256(path)})
        return files
    def write_manifest(files):
        manifest = {'schema': 1, 'package': name, 'version': args.version, 'status': 'test candidate; clean-install and sustained-live gates not yet passed externally',
                    'model': model_manifest['configuration'], 'runtime': model_manifest['runtime'], 'files': files}
        (folder / 'package-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        return sum(f['bytes'] for f in files) + (folder / 'package-manifest.json').stat().st_size
    # Pass 1: full inventory and sizes, so the proof report can state the package size and file count.
    files = inventory()
    extracted = write_manifest(files)
    report = {'package': name, 'files': len(files) + 1, 'extracted_bytes': extracted, 'folder': str(folder.relative_to(ROOT))}
    (ROOT / 'reports').mkdir(exist_ok=True)
    (ROOT / 'reports/package.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not args.no_docs_refresh:
        # Regenerate the documents with the package numbers, re-copy them, and re-hash only those entries.
        import subprocess
        subprocess.run([str(ROOT / '.venv/Scripts/python.exe'), str(ROOT / 'scripts/proof_report.py')], check=True, cwd=str(ROOT / 'scripts'))
        for target, source in docs.items():
            copy(ROOT / source, folder / target)
        by_path = {f['path']: f for f in files}
        for target in docs:
            path = folder / target
            by_path[target] = {'path': target, 'bytes': path.stat().st_size, 'sha256': sha256(path)}
        files = [by_path[k] for k in sorted(by_path)]
        extracted = write_manifest(files)
        report.update(files=len(files) + 1, extracted_bytes=extracted)
    if not args.no_archive:
        archive = dist / f'{name}.zip'
        started = time.perf_counter()
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for path in sorted(folder.rglob('*')):
                if path.is_file():
                    z.write(path, f'{name}/{path.relative_to(folder).as_posix()}')
        report.update(archive=str(archive.relative_to(ROOT)), compressed_bytes=archive.stat().st_size,
                      archive_sha256=sha256(archive), archive_seconds=time.perf_counter() - started, compression='zip deflate level 6')
    (ROOT / 'reports/package.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    (dist / f'{name}-release-info.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
