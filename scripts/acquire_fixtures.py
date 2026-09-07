"""Freeze fixtures before recognition from immutable dataset parquet revisions.

Build-only Python. Remote random reads avoid acquiring unrelated audio rows.
Selected audio files receive independent SHA-256 hashes in the manifest.
"""
import io
import json
import math
import random
import re
import struct
import urllib.request
import wave
from concurrent.futures import ThreadPoolExecutor
from bootstrap import ROOT, HEADERS, sha256
import pyarrow.parquet as pq
import soundfile as sf

REG = ROOT / 'regression'
REG.mkdir(exist_ok=True)
SOURCES = [
    ('mandarin', 'google/fleurs', 'cmn_hans_cn', '168de341b3db6859a9bac1c50a2ef5e3b47647e0', 695674033, 'CC-BY-4.0'),
    ('english', 'google/fleurs', 'en_us', '168de341b3db6859a9bac1c50a2ef5e3b47647e0', 401722686, 'CC-BY-4.0'),
    ('mixed', 'CAiRE/ASCEND', 'main', 'f2451e0d2f6e74ac864a9deb78c1a8ef3140176c', 105756434, 'CC-BY-SA-4.0'),
]

class RangeFile(io.RawIOBase):
    def __init__(self, url, size, cache):
        self.url, self.size, self.pos, self.cache = url, size, 0, cache
        cache.mkdir(parents=True, exist_ok=True)
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        if self.pos < 0: raise ValueError('Negative seek')
        return self.pos
    def read(self, size=-1):
        end = self.size if size < 0 else min(self.pos + size, self.size)
        result = bytearray()
        def fetch(start):
            stop = min(end, start + 8 * 1024 * 1024)
            cache = self.cache / f'{start}-{stop}.bin'
            if cache.exists():
                data = cache.read_bytes()
            else:
                req = urllib.request.Request(self.url, headers=dict(HEADERS, Range=f'bytes={start}-{stop-1}'))
                with urllib.request.urlopen(req, timeout=60) as r:
                    if r.status != 206 or r.headers.get('Content-Range') != f'bytes {start}-{stop-1}/{self.size}':
                        raise RuntimeError('Dataset server did not honor range')
                    data = r.read()
                if len(data) != stop-start: raise RuntimeError('Truncated dataset range')
                cache.write_bytes(data)
            return data
        with ThreadPoolExecutor(max_workers=4) as pool:
            for chunk in pool.map(fetch,range(self.pos,end,8*1024*1024)):
                result.extend(chunk)
        self.pos=end
        return bytes(result)

def main():
    manifest_path = REG / 'manifest.json'
    if manifest_path.exists():
        raise SystemExit('Fixture IDs already frozen; refusing to select a different test set.')
    records, audio_bytes = [], {}
    for group, dataset, config, revision, size, license_name in SOURCES:
        print(f'Reading frozen {group} test split', flush=True)
        url = f'https://huggingface.co/datasets/{dataset}/resolve/{revision}/{config}/test/0000.parquet'
        parquet = pq.ParquetFile(RangeFile(url, size, ROOT / '.cache/datasets' / group))
        selected, row_idx = 0, 0
        for batch in parquet.iter_batches(batch_size=100):
            for row in batch.to_pylist():
                text = row.get('raw_transcription', row.get('transcription', ''))
                mixed = bool(re.search(r'[\u3400-\u9fff]', text) and re.search(r'[A-Za-z]+', text))
                if group != 'mixed' or mixed:
                    fixture_id = f'{group}-{row_idx:04d}'
                    records.append({'id': fixture_id, 'category': group, 'row': row_idx,
                        'dataset': dataset, 'dataset_revision': revision, 'config': config,
                        'split': 'test', 'source_id': str(row.get('id', row.get('path', row_idx))),
                        'human_text': text, 'license': license_name, 'source_url': url})
                    audio_bytes[fixture_id] = row['audio']['bytes']
                    selected += 1
                row_idx += 1
                if selected == 6: break
            if selected == 6: break
        if selected != 6: raise RuntimeError(f'Not enough fixtures for {group}')
    selection = REG / 'selection.json'
    if selection.exists() and json.loads(selection.read_text('utf-8')) != records:
        raise RuntimeError('Previously frozen fixture selection differs')
    selection.write_text(json.dumps(records, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    for entry in records:
        samples, rate = sf.read(io.BytesIO(audio_bytes[entry['id']]), dtype='float32', always_2d=True)
        path = REG / (entry['id']+'.wav')
        sf.write(path, samples, rate, subtype='PCM_16')
        entry.update(path=path.name, sha256=sha256(path), duration_seconds=len(samples)/rate,
                     conversion='Decoded dataset audio, preserved sample rate/channels, PCM16 WAV')
    for i, (name, duration) in enumerate([('silence-short',0.5),('silence',2),('silence-long',15),('low-noise',3),('tone',2),('clicks',4)]):
        rng = random.Random(20260907+i)
        samples=[]
        for n in range(int(duration*16000)):
            value=0
            if name=='low-noise': value=rng.randint(-20,20)
            if name=='tone': value=int(1000*math.sin(2*math.pi*1000*n/16000))
            if name=='clicks' and n%8000 < 160: value=rng.randint(-1000,1000)
            samples.append(value)
        path=REG/(name+'.wav')
        with wave.open(str(path),'wb') as w:
            w.setparams((1,2,16000,0,'NONE','not compressed'))
            w.writeframes(struct.pack('<'+'h'*len(samples),*samples))
        records.append({'id':name,'category':'non-speech','path':path.name,'sha256':sha256(path),
            'human_text':'','license':'CC0-1.0','duration_seconds':duration,
            'source':'Deterministically generated by scripts/acquire_fixtures.py'})
    manifest={'schema':1,'selection_policy':'First six FLEURS test rows per language; first six ASCEND test rows containing Chinese characters and Latin words. Frozen before model evaluation.',
              'fixtures':records,'reference_status':'pending'}
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Frozen 24 regression fixtures.',flush=True)

if __name__ == '__main__': main()
