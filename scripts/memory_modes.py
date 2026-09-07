"""Measure peak process memory for the decoder weight-sharing configurations. Writes reports/memory.json."""
import json
import subprocess
from bootstrap import ROOT
from shipped import MODEL_DIR

exe = ROOT / 'build/Release/AsrWin.exe'
model = MODEL_DIR
FIXTURE = 'mandarin-0002'

def main():
    runs = []
    reference_tokens = None
    for mode in ['default', 'no-prepack', 'shared-prepack']:
        report = ROOT / 'traces/benchmarks' / f'memory-{mode}.json'
        report.parent.mkdir(parents=True, exist_ok=True)
        run = subprocess.run([str(exe), '--benchmark', str(ROOT / 'regression' / f'{FIXTURE}.wav'), '--model', str(model), '--memory-mode', mode,
                              '--skip-asset-check', '--report', str(report)], capture_output=True, text=True, encoding='utf-8')
        body = json.loads(report.read_text('utf-8'))
        t = body.get('transcription', {})
        if reference_tokens is None: reference_tokens = t.get('tokens')
        row = {'configuration': mode, 'exit': run.returncode, 'peak_working_set_bytes': t.get('memory', {}).get('peak_working_set_bytes'),
               'private_bytes': t.get('memory', {}).get('private_bytes'), 'model_load_seconds': body.get('model_load_seconds'),
               'total_seconds': t.get('total_seconds'), 'rtf': t.get('rtf'), 'tokens_identical': t.get('tokens') == reference_tokens, 'completion': t.get('completion')}
        runs.append(row)
        print(mode, row, flush=True)
    ok = [r for r in runs if r['exit'] == 0 and r['tokens_identical'] and r['completion'] == 'eos']
    # Smallest peak working set among configurations whose inference time stays within 10% of the fastest.
    fastest = min(ok, key=lambda r: r['total_seconds'])
    selected = min((r for r in ok if r['total_seconds'] <= fastest['total_seconds'] * 1.10), key=lambda r: r['peak_working_set_bytes'])
    result = {'fixture': FIXTURE, 'runs': runs, 'selected': selected['configuration'],
              'reason': 'smallest peak working set among configurations with identical tokens and inference time within 10% of the fastest'}
    (ROOT / 'reports/memory.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('selected:', result['selected'])

if __name__ == '__main__':
    main()
