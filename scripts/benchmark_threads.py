"""CPU thread-budget sweep on fixed fixtures. Writes reports/benchmark-threads.json."""
import argparse
import json
import statistics
import subprocess
from bootstrap import ROOT
from shipped import MODEL_DIR

exe = ROOT / 'build/Release/AsrWin.exe'
model = MODEL_DIR
FIXTURES = ['english-0002', 'mandarin-0002', 'mixed-0009']

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--threads', default='4,6,8,12,16')
    parser.add_argument('--memory-mode', default='shared-prepack')
    args = parser.parse_args()
    runs = []
    for threads in [int(x) for x in args.threads.split(',')]:
        for fixture in FIXTURES:
            report = ROOT / 'traces/benchmarks' / f'threads-{threads}-{fixture}.json'
            report.parent.mkdir(parents=True, exist_ok=True)
            run = subprocess.run([str(exe), '--benchmark', str(ROOT / 'regression' / f'{fixture}.wav'), '--model', str(model), '--threads', str(threads),
                                  '--memory-mode', args.memory_mode, '--skip-asset-check', '--report', str(report)], capture_output=True, text=True, encoding='utf-8')
            body = json.loads(report.read_text('utf-8'))
            t = body.get('transcription', {})
            row = {'threads': threads, 'fixture': fixture, 'exit': run.returncode, 'audio_seconds': t.get('audio_seconds'), 'total_seconds': t.get('total_seconds'),
                   'rtf': t.get('rtf'), 'encoder_seconds': t.get('encoder_seconds'), 'prefill_seconds': t.get('prefill_seconds'),
                   'decode_ms_per_token': (t.get('decode_seconds', 0) / max(1, len(t.get('tokens', []))) * 1000) if t else None,
                   'model_load_seconds': body.get('model_load_seconds'), 'completion': t.get('completion'), 'text': t.get('text')}
            runs.append(row)
            print(f"threads={threads} {fixture}: rtf {row['rtf']:.3f} decode {row['decode_ms_per_token']:.1f} ms/token prefill {row['prefill_seconds']:.3f} s encoder {row['encoder_seconds']:.3f} s load {row['model_load_seconds']:.1f} s" if row['rtf'] else f'threads={threads} {fixture}: FAILED {run.stderr[-300:]}', flush=True)
    summary = []
    for threads in sorted({r['threads'] for r in runs}):
        rows = [r for r in runs if r['threads'] == threads and r['rtf']]
        summary.append({'threads': threads, 'rtf': statistics.mean(r['rtf'] for r in rows), 'decode_ms_per_token': statistics.mean(r['decode_ms_per_token'] for r in rows),
                        'prefill_seconds': statistics.mean(r['prefill_seconds'] for r in rows), 'encoder_seconds': statistics.mean(r['encoder_seconds'] for r in rows),
                        'model_load_seconds': statistics.mean(r['model_load_seconds'] for r in rows)})
    best = min(summary, key=lambda s: s['rtf'])
    # Prefer the smallest budget within 5% of the best so the capture, UI and preprocessing threads keep headroom.
    selected = min((s for s in summary if s['rtf'] <= best['rtf'] * 1.05), key=lambda s: s['threads'])
    result = {'fixtures': FIXTURES, 'memory_mode': args.memory_mode, 'runs': runs, 'summary': summary, 'selected': selected['threads'],
              'reason': f"smallest thread count within 5% of the best mean RTF ({best['threads']} threads: {best['rtf']:.3f}); leaves headroom for capture, preprocessing and interface threads"}
    (ROOT / 'reports/benchmark-threads.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'summary': summary, 'selected': result['selected']}, indent=2))

if __name__ == '__main__':
    main()
