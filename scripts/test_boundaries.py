"""Full-model boundary proof: short input, encoder-window edges, utterance limit, repeated sessions.

Crops real fixture speech to exact mel-frame counts around the 8 s attention window
(800 frames = 104 encoder tokens) and the 15 s utterance limit, produces official
references for every crop with the unmodified oracle, runs the native executable on the
same files, and compares every stage numerically. Also runs the same file repeatedly,
interleaved with a different file, requiring identical tokens (no state carried between
generations). Build-time only.
"""
import json
import subprocess
import sys
import numpy as np
import soundfile as sf
from bootstrap import ROOT
from shipped import MODEL_DIR

exe = ROOT / 'build/Release/AsrWin.exe'
model = MODEL_DIR
python = ROOT / '.venv/Scripts/python.exe'
work = ROOT / 'traces/boundaries'
work.mkdir(parents=True, exist_ok=True)

def crop(source, name, samples, pad_to=None):
    audio, rate = sf.read(ROOT / 'regression' / source, dtype='float32', always_2d=True)
    mono = audio.mean(axis=1, dtype=np.float32)
    if rate != 16000:
        import librosa
        mono = librosa.resample(mono, orig_sr=rate, target_sr=16000).astype(np.float32)
    mono = mono[:samples]
    if pad_to:
        mono = np.pad(mono, (0, max(0, pad_to - len(mono))))
    path = work / f'{name}.wav'
    sf.write(path, mono, 16000, subtype='PCM_16')
    return path

cases = [
    ('short-1sample', crop('english-0003.wav', 'short-1sample', 1)),                # padded to 0.5 s by both sides
    ('short-half-second', crop('english-0003.wav', 'short-half-second', 8000)),
    ('window-799-frames', crop('english-0002.wav', 'window-799-frames', 799 * 160)),  # 103 tokens, one attention window
    ('window-800-frames', crop('english-0002.wav', 'window-800-frames', 800 * 160)),  # 104 tokens, exactly one window
    ('window-801-frames', crop('english-0002.wav', 'window-801-frames', 801 * 160)),  # 105 tokens, second window opens
    ('window-1600-frames', crop('mandarin-0003.wav', 'window-1600-frames', 1600 * 160)),  # two full windows
    ('limit-15s', crop('mandarin-0003.wav', 'limit-15s', 240000)),                    # utterance limit (1500 frames, 195 tokens)
    ('limit-15s-padded', crop('english-0004.wav', 'limit-15s-padded', 240000, pad_to=240000)),  # short speech + trailing silence
]
ids = [name for name, _ in cases]
print('Generating official references for', ids, flush=True)
run = subprocess.run([str(python), str(ROOT / 'scripts/reference.py'), '--threads', '8', *sum((['--extra', str(p)] for _, p in cases), [])],
                     capture_output=True, text=True, encoding='utf-8')
if run.returncode:
    print(run.stdout[-3000:], run.stderr[-3000:])
    raise SystemExit('Reference generation failed')
results = []
for name, path in cases:
    trace = ROOT / 'traces/native' / name
    native = subprocess.run([str(exe), '--benchmark', str(path), '--model', str(model), '--trace', str(trace), '--report', str(work / f'{name}.native.json')],
                            capture_output=True, text=True, encoding='utf-8')
    compare = subprocess.run([str(python), str(ROOT / 'scripts/compare_traces.py'), name, '--report', str(ROOT / 'reports/stages-boundaries' / f'{name}.json')],
                             capture_output=True, text=True, encoding='utf-8')
    stage = json.loads((ROOT / 'reports/stages-boundaries' / f'{name}.json').read_text('utf-8'))
    reference = json.loads((ROOT / 'regression/reference-extra' / f'{name}.json').read_text('utf-8'))
    report = json.loads((work / f'{name}.native.json').read_text('utf-8')) if (work / f'{name}.native.json').exists() else {}
    observed = report.get('transcription', {})
    row = {'case': name, 'native_exit': native.returncode, 'reference_text': reference['text'], 'native_text': observed.get('text'),
           'reference_completion': reference['completion'], 'native_completion': observed.get('completion'),
           'audio_tokens': json.loads((ROOT / 'traces/reference' / name / 'prompt.json').read_text('utf-8'))['audio_tokens'],
           'tokens_exact': stage.get('tokens_exact'), 'stages_success': stage.get('success'),
           'stage_max_errors': {k: v.get('max_absolute_error') for k, v in stage.get('stages', {}).items()},
           'success': native.returncode == 0 and stage.get('success') is True and observed.get('completion') == 'eos'}
    print(name, 'PASS' if row['success'] else 'FAIL', row['audio_tokens'], 'tokens;', row['native_text'], flush=True)
    results.append(row)
# Repeated sessions and cache reset: same audio five times, interleaved with another file.
repeat = subprocess.run([str(exe), '--benchmark', str(ROOT / 'regression/mixed-0009.wav'), '--interleave', str(ROOT / 'regression/english-0004.wav'),
                         '--repeat', '5', '--model', str(model), '--report', str(work / 'repeat.json')], capture_output=True, text=True, encoding='utf-8')
repeat_report = json.loads((work / 'repeat.json').read_text('utf-8'))
reference_mixed = json.loads((ROOT / 'regression/reference/mixed-0009.json').read_text('utf-8'))
repeat_row = {'case': 'repeat-interleaved', 'exit': repeat.returncode, 'runs': len(repeat_report.get('repeat_runs', [])),
              'tokens_identical_across_runs': repeat_report.get('repeat_tokens_identical'),
              'matches_reference_tokens': all(r['tokens'] == reference_mixed['tokens'] for r in repeat_report.get('repeat_runs', []) if r['file'] == 'input'),
              'success': repeat.returncode == 0 and repeat_report.get('repeat_tokens_identical') is True}
print('repeat-interleaved', 'PASS' if repeat_row['success'] else 'FAIL', flush=True)
results.append(repeat_row)
summary = {'success': all(r['success'] for r in results), 'cases': results}
(ROOT / 'reports/boundaries.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('boundaries:', 'PASS' if summary['success'] else 'FAIL')
raise SystemExit(0 if summary['success'] else 1)
