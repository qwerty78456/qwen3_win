"""Collect and judge the DirectML evidence for the shipped models; write reports/directml.json.

For each model (`--models default,large`) on the chosen adapter this runs, unless skipped:
  1. `--verify <regression manifest> --provider directml --trace traces/native-directml[<suffix>]`
     -> reports/verification-directml[<suffix>].json (24 fixtures, tokens compared with the official oracle);
  2. `compare_traces.py <fixture> --provider directml [--model-key large]` for every fixture
     -> reports/stages-directml[<suffix>]/<fixture>.json (numerical stages against the oracle traces);
  3. four `--simulate-live` replays -> reports/live-<fixture>-directml[<suffix>].json;
and reads the sustained loopback capture produced separately by
`live_capture_test.py --provider directml [--large-model] --playlist playlist-60 --name 60min-load2-directml[<suffix>] --load 2`.

Every criterion is judged against the CPU baseline of the same model (reports/verification.json for the
default model, reports/proof-1.7b/verification.json for the large one) and written with pass/fail flags.
The script exits nonzero unless every criterion of every requested model passes; `shipped-model.json`
`directml.validated` must only be set to true from a passing report. Build-time only.
"""
import argparse
import datetime
import json
import subprocess
import time
from bootstrap import ROOT, sha256
from shipped import CONFIG, DIRECTML_VALIDATED, MODELS, model_config

exe = ROOT / 'build/Release/AsrWin.exe'
python = ROOT / '.venv/Scripts/python.exe'
LIVE_FIXTURES = ['english-0002', 'mandarin-0002', 'mandarin-0003', 'mixed-0009']
CPU_BASELINES = {'default': 'reports/verification.json', 'large': 'reports/proof-1.7b/verification.json'}

def load(rel):
    path = ROOT / rel
    return json.loads(path.read_text('utf-8')) if path.exists() else None

def run(command, log, timeout):
    print('>', ' '.join(str(c) for c in command), flush=True)
    started = time.perf_counter()
    with open(log, 'w', encoding='utf-8') as f:
        proc = subprocess.run([str(c) for c in command], stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, time.perf_counter() - started

def pick_adapter(selector, skip_check):
    listing_path = ROOT / 'reports/adapters.json'
    code, _ = run([exe, '--adapters', '--model', ROOT / MODELS['default']['model_dir'], '--large-model-dir', ROOT / MODELS['large']['model_dir'] if 'large' in MODELS else '', '--report', listing_path] if 'large' in MODELS
                  else [exe, '--adapters', '--model', ROOT / MODELS['default']['model_dir'], '--report', listing_path], ROOT / 'reports/adapters.log', 120)
    if code != 0: raise SystemExit('--adapters failed; see reports/adapters.log')
    listing = json.loads(listing_path.read_text('utf-8'))
    if selector == 'default':
        chosen = [a for a in listing['adapters'] if a.get('default')]
    else:
        chosen = [a for a in listing['adapters'] if a['directml_index'] == int(selector)]
    if not chosen: raise SystemExit(f'No adapter matches --adapter {selector}; see reports/adapters.json')
    if not chosen[0]['eligible'] and not skip_check: raise SystemExit(f"Adapter {chosen[0]['directml_index']} is not eligible: {chosen[0]['reason']}")
    return chosen[0], listing

def percentile_stats(report):
    live = report['live']
    return {'success': report.get('success'), 'final_count': live['final_count'], 'provisional_p95': live['provisional_lag_seconds']['p95'], 'provisional_max': live['provisional_lag_seconds']['max'],
            'first_caption_p95': live['first_caption_lag_seconds']['p95'], 'finalization_p95': live['finalization_delay_seconds']['p95'], 'max_backlog': live['pipeline']['max_backlog_seconds'],
            'suspensions': live['pipeline'].get('suspensions'), 'stop_reason': live['pipeline'].get('stop_reason'), 'peak_working_set_bytes': live['pipeline']['memory']['peak_working_set_bytes'],
            'private_bytes': live['pipeline']['memory'].get('private_bytes'), 'wall_seconds': live['pipeline'].get('wall_seconds')}

def file_summary(report, fixture_ids):
    rows = {r['id']: r for r in report['fixtures']}
    obs = {k: rows[k]['observed'] for k in fixture_ids if k in rows and rows[k].get('observed')}
    speech = [k for k in obs if not k.startswith(('silence', 'low-noise', 'tone', 'clicks'))]
    tokens = sum(len(obs[k]['tokens']) for k in obs)
    return {'fixtures': len(obs), 'passed': sum(1 for k in fixture_ids if rows.get(k, {}).get('success')), 'tokens_exact': sum(1 for k in fixture_ids if rows.get(k, {}).get('tokens_exact')),
            'total_seconds': sum(obs[k]['total_seconds'] for k in obs), 'speech_total_seconds': sum(obs[k]['total_seconds'] for k in speech),
            'decode_ms_per_token': sum(obs[k]['decode_seconds'] for k in obs) / max(1, tokens) * 1000, 'mean_encoder_seconds': sum(obs[k]['encoder_seconds'] for k in obs) / max(1, len(obs)),
            'mean_prefill_seconds': sum(obs[k]['prefill_seconds'] for k in obs) / max(1, len(obs)), 'mean_rtf_speech': sum(obs[k]['rtf'] for k in speech) / max(1, len(speech)),
            'max_rtf_speech': max((obs[k]['rtf'] for k in speech), default=None), 'model_load_seconds': report.get('model_load_seconds'),
            'peak_working_set_bytes': max((obs[k]['memory']['peak_working_set_bytes'] for k in obs), default=None), 'max_private_bytes': max((obs[k]['memory']['private_bytes'] for k in obs), default=None),
            'engine': {k: report['engine'].get(k) for k in ('provider', 'threads', 'adapter_index', 'adapter_name', 'adapter_luid', 'adapter_driver_version', 'adapter_luid_confirmed', 'large_model', 'runtime')},
            'gpu_memory': report.get('gpu_memory')}

def judge_model(key, args, adapter, experimental):
    cfg = model_config(key); suffix = cfg['suffix']; name = cfg['name']
    model_dir = ROOT / cfg['model_dir']
    manifest_path = ROOT / cfg['regression_manifest']
    manifest = json.loads(manifest_path.read_text('utf-8')); fixture_ids = [f['id'] for f in manifest['fixtures']]
    large = ['--large-model'] if key == 'large' else []
    common = ['--model', model_dir, *large, '--provider', 'directml', '--adapter', str(adapter['directml_index']), *experimental] + (['--skip-adapter-check'] if args.skip_adapter_check else [])
    out = {'name': name, 'model_dir': cfg['model_dir'], 'suffix': suffix, 'criteria': {}, 'measurements': {}}
    model_manifest = load(cfg['model_dir'] + '/manifest.json')
    out['model'] = {'configuration': model_manifest['configuration'], 'manifest_sha256': sha256(model_dir / 'manifest.json'), 'variant': model_manifest['variant'],
                    'weight_bytes': sum(i['bytes'] for i in model_manifest['files'] if i['path'] in ('encoder.onnx', 'decoder_init.onnx', 'decoder_step.onnx', 'decoder_weights.data'))}
    # 1. regression
    verification_path = ROOT / f'reports/verification-directml{suffix}.json'
    if not args.skip_verify:
        code, seconds = run([exe, '--verify', manifest_path, *common, '--trace', ROOT / f'traces/native-directml{suffix}', '--report', verification_path], ROOT / f'reports/verification-directml{suffix}.log', 3600)
        out['measurements']['verify_exit'] = code; out['measurements']['verify_wall_seconds'] = seconds
    verification = load(verification_path.relative_to(ROOT).as_posix())
    baseline = load(CPU_BASELINES[key])
    if not verification or 'fixtures' not in verification: raise SystemExit(f'{name}: no DirectML verification report ({verification_path})')
    if not baseline or 'fixtures' not in baseline: raise SystemExit(f'{name}: no CPU baseline ({CPU_BASELINES[key]})')
    dml = file_summary(verification, fixture_ids); cpu = file_summary(baseline, fixture_ids)
    out['criteria']['regression_24_pass'] = {'success': bool(verification.get('success')) and dml['passed'] == 24 and dml['tokens_exact'] == 24, 'passed': dml['passed'], 'tokens_exact': dml['tokens_exact'], 'report': verification_path.relative_to(ROOT).as_posix()}
    cpu_rows = {r['id']: r for r in baseline['fixtures']}
    differing = [r['id'] for r in verification['fixtures'] if r.get('observed', {}).get('tokens') != cpu_rows.get(r['id'], {}).get('observed', {}).get('tokens')]
    out['criteria']['tokens_identical_to_cpu'] = {'success': not differing, 'differing_fixtures': differing, 'cpu_baseline': CPU_BASELINES[key]}
    # 2. stages
    stage_dir = ROOT / f'reports/stages-directml{suffix}'
    if not args.skip_stages:
        for fixture in fixture_ids:
            run([python, ROOT / 'scripts/compare_traces.py', fixture, '--provider', 'directml', '--model-key', key], ROOT / f'reports/stages-directml{suffix}.log', 600)
    stages = {p.stem: json.loads(p.read_text('utf-8')) for p in sorted(stage_dir.glob('*.json'))} if stage_dir.exists() else {}
    failed = [k for k in fixture_ids if not stages.get(k, {}).get('success')]
    overrides = sorted({f"{k}:{s}" for k, v in stages.items() for s, item in v.get('stages', {}).items() if item.get('tolerance_source', 'base') != 'base'})
    out['criteria']['stages_within_tolerance'] = {'success': len(stages) == 24 and not failed, 'passed': 24 - len(failed) if stages else 0, 'failed': failed, 'overrides_used': overrides, 'report_dir': stage_dir.relative_to(ROOT).as_posix() + '/'}
    # 3. end-to-end gain
    gain = (cpu['total_seconds'] - dml['total_seconds']) / cpu['total_seconds'] if cpu['total_seconds'] else None
    gain_speech = (cpu['speech_total_seconds'] - dml['speech_total_seconds']) / cpu['speech_total_seconds'] if cpu['speech_total_seconds'] else None
    out['criteria']['end_to_end_gain'] = {'success': gain is not None and gain >= args.min_gain, 'minimum': args.min_gain, 'gain_all_fixtures': gain, 'gain_speech': gain_speech,
                                          'cpu_total_seconds': cpu['total_seconds'], 'directml_total_seconds': dml['total_seconds'], 'cpu_threads': cpu['engine']['threads']}
    out['measurements']['file'] = {'cpu': cpu, 'directml': dml, 'per_fixture': [{'id': r['id'], 'cpu_seconds': cpu_rows[r['id']]['observed']['total_seconds'], 'directml_seconds': r['observed']['total_seconds'],
                                    'tokens_identical': r['observed']['tokens'] == cpu_rows[r['id']]['observed']['tokens'], 'directml_encoder_seconds': r['observed']['encoder_seconds'], 'cpu_encoder_seconds': cpu_rows[r['id']]['observed']['encoder_seconds']}
                                    for r in verification['fixtures'] if r.get('observed') and cpu_rows.get(r['id'], {}).get('observed')]}
    # 4. simulated live runs
    live = {}
    for fixture in LIVE_FIXTURES:
        report_path = ROOT / f'reports/live-{fixture}-directml{suffix}.json'
        if not args.skip_live:
            run([exe, '--benchmark', ROOT / f'regression/{fixture}.wav', '--simulate-live', *common, '--report', report_path], ROOT / f'reports/live-{fixture}-directml{suffix}.log', 900)
        report = load(report_path.relative_to(ROOT).as_posix())
        cpu_report = load(f'reports/live-{fixture}.json') if key == 'default' else load(f'reports/proof-1.7b/live-{fixture}.json')
        live[fixture] = {'success': bool(report and report.get('success') and report['live']['provisional_lag_seconds']['p95'] <= 4.0),
                         'directml': percentile_stats(report) if report and 'live' in report else None, 'cpu': percentile_stats(cpu_report) if cpu_report and 'live' in cpu_report else None,
                         'warm_up_seconds': report.get('engine', {}).get('warm_up_seconds') if report else None}
    out['criteria']['live_runs_pass'] = {'success': all(v['success'] for v in live.values()), 'runs': live}
    # 5. sustained loopback capture (produced by live_capture_test.py)
    capture_name = f'60min-load2-directml{suffix}'
    capture = load(f'reports/capture-{capture_name}.json'); scored = load(f'reports/capture-{capture_name}-scored.json')
    sustained = {'report': f'reports/capture-{capture_name}.json', 'present': bool(capture and 'live' in capture)}
    if sustained['present']:
        stats = percentile_stats(capture); engine = capture.get('engine', {})
        sustained.update(stats, provider=engine.get('provider'), adapter_luid=engine.get('adapter_luid'), adapter_driver_version=engine.get('adapter_driver_version'), gpu_memory=capture.get('gpu_memory'),
                         background_load_processes=scored.get('background_load_processes') if scored else None, no_audio_loss=capture['live'].get('no_audio_loss'), overload_stop=capture['live'].get('overload_stop'),
                         accuracy=scored.get('summary') if scored else None)
        sustained['success'] = bool(capture.get('success')) and str(engine.get('provider', '')).startswith('DirectML') and engine.get('adapter_luid') == adapter['luid'] \
            and (stats['wall_seconds'] or 0) >= 3500 and stats['provisional_p95'] <= 4.0 and not capture['live'].get('overload_stop') and capture['live'].get('no_audio_loss') \
            and (scored or {}).get('background_load_processes') == 2 and stats['peak_working_set_bytes'] <= 16_000_000_000
    else:
        sustained['success'] = False
    out['criteria']['sustained_load'] = sustained
    # 6. adapter identity across the runs
    identities = {'verification': {k: verification['engine'].get(k) for k in ('adapter_index', 'adapter_name', 'adapter_luid', 'adapter_driver_version', 'adapter_luid_confirmed')}}
    for fixture, v in live.items():
        r = load(f'reports/live-{fixture}-directml{suffix}.json')
        if r: identities[f'live-{fixture}'] = {k: r['engine'].get(k) for k in ('adapter_index', 'adapter_name', 'adapter_luid', 'adapter_driver_version', 'adapter_luid_confirmed')}
    if sustained['present']: identities['sustained'] = {k: capture['engine'].get(k) for k in ('adapter_index', 'adapter_name', 'adapter_luid', 'adapter_driver_version', 'adapter_luid_confirmed')}
    consistent = all(i.get('adapter_luid') == adapter['luid'] and i.get('adapter_driver_version') == adapter['driver_version'] and i.get('adapter_luid_confirmed') for i in identities.values())
    out['criteria']['adapter_identity'] = {'success': consistent, 'runs': identities}
    # 7. held-out token identity for the large model (evaluate_variants.py candidate)
    if key == 'large':
        variants = load('reports/variant-evaluation.json') or {}
        decision = variants.get('decisions', {}).get('1.7b-directml')
        out['criteria']['heldout_tokens_identical_to_cpu'] = {'success': bool(decision and decision.get('accepted') and decision.get('tokens_identical_to_cpu_baseline')), 'decision': decision, 'report': 'reports/variant-evaluation.json'}
    out['measurements']['gpu_memory'] = {'verification': verification.get('gpu_memory'), 'sustained': sustained.get('gpu_memory')}
    out['success'] = all(c['success'] for c in out['criteria'].values())
    return out

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--adapter', default='default')
    parser.add_argument('--models', default='default,large' if 'large' in MODELS else 'default', help='comma-separated model keys (default, large)')
    parser.add_argument('--skip-verify', action='store_true'); parser.add_argument('--skip-stages', action='store_true'); parser.add_argument('--skip-live', action='store_true')
    parser.add_argument('--skip-adapter-check', action='store_true')
    parser.add_argument('--min-gain', type=float, default=0.15)
    args = parser.parse_args()
    if not exe.exists(): raise SystemExit('Build the executable first')
    baseline = load('reports/verification.json')
    if not baseline or not baseline.get('success') or len(baseline.get('fixtures', [])) != 24 or baseline['engine']['provider'] != 'CPU':
        raise SystemExit('reports/verification.json must be a passing 24-fixture CPU regression of the current binary')
    if (ROOT / 'reports/verification.json').stat().st_mtime < exe.stat().st_mtime and not args.skip_verify:
        print('warning: reports/verification.json is older than the executable; rerun the CPU regression before judging the gain', flush=True)
    adapter, listing = pick_adapter(args.adapter, args.skip_adapter_check)
    experimental = [] if DIRECTML_VALIDATED else ['--experimental-directml']
    models = {}
    for key in args.models.split(','):
        models[model_config(key)['name']] = judge_model(key, args, adapter, experimental)
    success = all(m['success'] for m in models.values())
    failed = {name: [c for c, v in m['criteria'].items() if not v['success']] for name, m in models.items() if not m['success']}
    default_name = model_config('default')['name']
    default = models.get(default_name) or next(iter(models.values()))
    gain = default['criteria']['end_to_end_gain']; sustained = default['criteria']['sustained_load']; live = default['criteria']['live_runs_pass']['runs'].get('english-0002', {})
    if success:
        summary = (f"DirectML on {adapter['name']} (driver {adapter['driver_version']}, DXGI adapter {adapter['directml_index']}, LUID {adapter['luid']}), model {default['name']}: "
                   f"{default['criteria']['regression_24_pass']['passed']}/24 fixtures pass with token sequences identical to the CPU run and to the oracle, "
                   f"{default['criteria']['stages_within_tolerance']['passed']}/24 numerical stages within tolerance ({'no overrides' if not default['criteria']['stages_within_tolerance']['overrides_used'] else 'overrides: ' + ', '.join(default['criteria']['stages_within_tolerance']['overrides_used'])}), "
                   f"file transcription {gain['gain_all_fixtures'] * 100:.1f} % faster end to end ({gain['cpu_total_seconds']:.2f} s -> {gain['directml_total_seconds']:.2f} s), "
                   f"simulated-live provisional lag p95 {live.get('cpu', {}).get('provisional_p95', float('nan')):.2f} s -> {live.get('directml', {}).get('provisional_p95', float('nan')):.2f} s (english-0002), "
                   f"60-minute loopback with 2 load processes passed (provisional p95 {sustained.get('provisional_p95', float('nan')):.2f} s, {sustained.get('suspensions')} suspensions)"
                   + (f"; the large model ({model_config('large')['configuration']}) passed the same criteria on this adapter" if 'large' in MODELS and models.get(model_config('large')['name'], {}).get('success') else '')
                   + '. Other adapters and drivers are untested.')
    else:
        summary = 'DirectML NOT validated: ' + '; '.join(f"{name}: {', '.join(c)}" for name, c in failed.items())
    result = {'schema': 1, 'generated_at': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'), 'success': success, 'summary': summary,
              'binary': {'path': 'build/Release/AsrWin.exe', 'sha256': sha256(exe), 'directml_validated_at_build': DIRECTML_VALIDATED, 'runtime': baseline['engine'].get('runtime')},
              'adapter': adapter, 'adapters': listing, 'default_provider': CONFIG.get('default_provider'), 'models': models, 'failed_criteria': failed}
    (ROOT / 'reports/directml.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'success': success, 'summary': summary, 'failed_criteria': failed}, ensure_ascii=False, indent=2))
    raise SystemExit(0 if success else 2)

if __name__ == '__main__':
    main()
