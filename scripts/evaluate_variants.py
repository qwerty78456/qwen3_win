"""Decide the shipped model configuration on the held-out set (never on the regression set).

Runs `--verify evaluation/manifest.json` for each candidate variant, compares pooled human
error rates per language category (Mandarin CER, English WER, mixed MER) with the FP32
baseline, and applies the acceptance rule: at most one percentage point absolute increase per
category and no new silence failures. Also records file-transcription speed on the same set.
The `1.7b-directml` candidate runs the 1.7B graphs on the DirectML adapter (`--adapter`) and is
additionally required to produce token sequences identical to the CPU baseline on every fixture.
Writes reports/variant-evaluation.json. Build-time only.
"""
import argparse
import json
import subprocess
from bootstrap import ROOT
from shipped import DIRECTML_VALIDATED

exe = ROOT / 'build/Release/AsrWin.exe'
# Candidate name -> (model directory, decoder variant flag, provider, held-out manifest)
CANDIDATES = {'fp32': ('models/qwen3-asr-1.7b-fp32', 'fp32', 'cpu', 'evaluation/manifest-1.7b.json'),
              'int4': ('models/qwen3-asr-1.7b-fp32', 'int4', 'cpu', 'evaluation/manifest-1.7b.json'),
              '0.6b': ('models/qwen3-asr-0.6b-fp32', 'fp32', 'cpu', 'evaluation/manifest.json'),
              '1.7b-directml': ('models/qwen3-asr-1.7b-fp32', 'fp32', 'directml', 'evaluation/manifest-1.7b.json')}
REUSE = False
ADAPTER = 'default'

def run(variant):
    model_dir, flag, provider, manifest_name = CANDIDATES[variant]
    model = ROOT / model_dir
    manifest_path = ROOT / manifest_name
    if not manifest_path.exists(): manifest_path = ROOT / 'evaluation/manifest.json'  # references of the 1.7B oracle before prepare_large_model_references.py existed
    report = ROOT / 'reports' / f'evaluation-{variant}.json'
    # Development benchmark: the frozen model manifest lists only the shipped variant, so the per-load
    # asset check is skipped here (assets were verified against publisher hashes at acquisition).
    class Done: returncode = 0; stderr = ''
    proc = Done()
    if not (REUSE and report.exists() and 'fixtures' in json.loads(report.read_text('utf-8'))):
        command = [str(exe), '--verify', str(manifest_path), '--variant', flag, '--model', str(model), '--skip-asset-check', '--report', str(report), '--provider', provider]
        if provider == 'directml':
            command += ['--adapter', ADAPTER, '--large-model'] + ([] if DIRECTML_VALIDATED else ['--experimental-directml'])
        proc = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
    body = json.loads(report.read_text('utf-8'))
    if 'fixtures' not in body:
        raise RuntimeError(f'{variant}: verification did not run: {body.get("error")} {proc.stderr[-500:]}')
    categories = {}
    manifest = json.loads(manifest_path.read_text('utf-8'))
    cat_of = {f['id']: f['category'] for f in manifest['fixtures']}
    silence_failures = []
    rtf = []
    for row in body['fixtures']:
        cat = cat_of[row['id']]
        obs = row.get('observed', {})
        if cat == 'non-speech':
            if obs.get('text', None) != '' or obs.get('completion') != 'eos':
                silence_failures.append(row['id'])
            continue
        c = categories.setdefault(cat, {'edits': 0, 'units': 0, 'fixtures': 0, 'oracle_transcript_matches': 0, 'eos': 0})
        he = row.get('human_error', {})
        c['edits'] += he.get('edits', 0); c['units'] += he.get('reference_units', 0); c['fixtures'] += 1
        c['oracle_transcript_matches'] += bool(row.get('normalized_transcript_match'))
        c['eos'] += obs.get('completion') == 'eos'
        if obs.get('rtf'): rtf.append(obs['rtf'])
    for c in categories.values():
        c['error_rate'] = c['edits'] / c['units'] if c['units'] else None
    engine = body.get('engine', {})
    return {'variant': variant, 'model': model_dir, 'decoder': flag, 'provider': provider, 'engine_provider': engine.get('provider'), 'adapter_name': engine.get('adapter_name'),
            'adapter_driver_version': engine.get('adapter_driver_version'), 'manifest': manifest_path.relative_to(ROOT).as_posix(), 'exit': proc.returncode, 'categories': categories, 'silence_failures': silence_failures,
            'texts': {r['id']: r.get('observed', {}).get('text') for r in body['fixtures']}, 'edits': {r['id']: r.get('human_error', {}).get('edits') for r in body['fixtures']},
            'tokens': {r['id']: r.get('observed', {}).get('tokens') for r in body['fixtures']},
            'mean_rtf': sum(rtf) / len(rtf) if rtf else None, 'model_load_seconds': body.get('model_load_seconds'),
            'peak_working_set_bytes': max((r.get('observed', {}).get('memory', {}).get('peak_working_set_bytes', 0) for r in body['fixtures']), default=None),
            'gpu_memory': body.get('gpu_memory'), 'report': str(report.relative_to(ROOT))}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--variants', default='fp32,int4')
    parser.add_argument('--reuse', action='store_true', help='reuse existing reports/evaluation-<variant>.json instead of re-running that candidate')
    parser.add_argument('--adapter', default='default', help='DirectML adapter for the *-directml candidates')
    args = parser.parse_args()
    global REUSE, ADAPTER
    REUSE = args.reuse; ADAPTER = args.adapter
    results = {v: run(v) for v in args.variants.split(',')}
    baseline = results['fp32']
    decisions = {}
    for variant, r in results.items():
        if variant == 'fp32':
            continue
        deltas = {}
        accepted = r['exit'] in (0, 2) and not (set(r['silence_failures']) - set(baseline['silence_failures']))
        for cat, c in r['categories'].items():
            base = baseline['categories'][cat]['error_rate']
            deltas[cat] = {'baseline': base, 'candidate': c['error_rate'], 'delta_pp': (c['error_rate'] - base) * 100}
            if deltas[cat]['delta_pp'] > 1.0:
                accepted = False
        differing = [{'id': k, 'baseline_text': baseline['texts'].get(k), 'candidate_text': v, 'baseline_edits': baseline['edits'].get(k), 'candidate_edits': r['edits'].get(k)} for k, v in r['texts'].items() if v != baseline['texts'].get(k)]
        decisions[variant] = {'accepted': accepted, 'deltas_percentage_points': deltas, 'new_silence_failures': sorted(set(r['silence_failures']) - set(baseline['silence_failures'])), 'fixtures_differing_from_baseline': differing}
        if r['provider'] == 'directml' and r['model'] == baseline['model'] and r['decoder'] == baseline['decoder']:
            different_tokens = sorted(k for k, v in r['tokens'].items() if v != baseline['tokens'].get(k))
            decisions[variant]['tokens_identical_to_cpu_baseline'] = not different_tokens
            decisions[variant]['fixtures_with_different_tokens'] = different_tokens
            decisions[variant]['accepted'] = accepted and not different_tokens
    for r in results.values():
        r.pop('texts', None); r.pop('edits', None); r.pop('tokens', None)
    result = {'rule': 'candidate accepted when every category increases by at most 1.0 percentage point absolute and no new non-speech fixture produces output; a DirectML candidate of the baseline model must also produce identical token sequences on every held-out fixture',
              'results': results, 'decisions': decisions}
    (ROOT / 'reports/variant-evaluation.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'baseline': {k: v['error_rate'] for k, v in baseline['categories'].items()}, 'decisions': decisions,
                      'rtf': {k: v['mean_rtf'] for k, v in results.items()}}, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
