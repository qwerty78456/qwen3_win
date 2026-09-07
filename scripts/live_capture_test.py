"""Real loopback test: play a fixture playlist to the default endpoint while AsrWin captures it.

ffplay renders traces/live/<playlist>.wav through the default playback device (no window),
`AsrWin.exe --capture` records the endpoint through WASAPI loopback for the playlist duration,
and the finals are scored against the fixture human transcripts and the official references
by time alignment. Optional background load (--load N) keeps N cores busy to imitate office work.
Build-time only; writes reports/capture-<name>.json (the raw AsrWin report) and
reports/capture-<name>-scored.json.
"""
import argparse
import json
import multiprocessing
import re
import subprocess
import time
import unicodedata
from pathlib import Path
from bootstrap import ROOT
from shipped import MODEL_DIR, VARIANT

def tokens(text, characters):
    text = unicodedata.normalize('NFKC', text)
    out, word = [], ''
    for ch in text:
        cp = ord(ch)
        chinese = 0x3400 <= cp <= 0x9fff or 0xf900 <= cp <= 0xfaff or 0x20000 <= cp <= 0x323af
        if chinese:
            if word: out.append(word); word = ''
            out.append(ch)
        elif ch.isascii() and ch.isalnum():
            if characters: out.append(ch.lower())
            else: word += ch.lower()
        elif ch not in "'’":
            if word: out.append(word); word = ''
    if word: out.append(word)
    return out

def edits(a, b):
    prev = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        cur = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1]))
        prev = cur
    return prev[-1]

def busy(seconds):
    end = time.time() + seconds
    x = 0
    while time.time() < end:
        x = (x * 1103515245 + 12345) & 0x7fffffff

def main():
    import ctypes
    # Keep the display and its HDMI/DP audio endpoint awake for the duration of the test (per-process request, no system setting change).
    ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000002 | 0x00000001)
    parser = argparse.ArgumentParser()
    parser.add_argument('--playlist', default='playlist-1x')
    parser.add_argument('--name', default='loopback')
    parser.add_argument('--device', default='default')
    parser.add_argument('--load', type=int, default=0, help='background busy processes during the run')
    parser.add_argument('--extra-seconds', type=float, default=8.0, help='capture time after the playlist ends')
    parser.add_argument('--volume', type=int, default=100)
    parser.add_argument('--variant', default=VARIANT)
    parser.add_argument('--rescore', action='store_true', help='score an existing reports/capture-<name>.json without capturing again')
    args = parser.parse_args()
    playlist = ROOT / 'traces/live' / f'{args.playlist}.wav'
    schedule = json.loads((ROOT / 'traces/live' / f'{args.playlist}.json').read_text('utf-8'))
    duration = schedule['duration_seconds'] + args.extra_seconds
    report = ROOT / 'reports' / f'capture-{args.name}.json'
    exe = ROOT / 'build/Release/AsrWin.exe'
    started_line = None
    if not args.rescore:
        capture = subprocess.Popen([str(exe), '--capture', '--device', args.device, '--duration', str(duration), '--model', str(MODEL_DIR),
                                    '--variant', args.variant, '--report', str(report), '--transcript', str(ROOT / 'traces/live' / f'{args.name}.txt')],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        # Wait for the model to load and capture to start before playing.
        for line in capture.stdout:
            print(line.rstrip(), flush=True)
            if line.startswith('Capturing playback from'):
                started_line = line.strip(); break
        if started_line is None:
            capture.wait(); raise SystemExit('Capture did not start')
        time.sleep(1.0)
        loads = [multiprocessing.Process(target=busy, args=(duration,)) for _ in range(args.load)]
        for p in loads: p.start()
        player = subprocess.Popen(['ffplay', '-nodisp', '-autoexit', '-loglevel', 'quiet', '-volume', str(args.volume), str(playlist)])
        for line in capture.stdout:
            print(line.rstrip(), flush=True)
        capture.wait()
        if player.poll() is None: player.terminate()  # capture ended early (device lost, overload): never leave audio playing
        player.wait()
        for p in loads: p.join()
    body = json.loads(report.read_text('utf-8'))
    events = body['live']['events']
    finals = [e for e in events if e['kind'] == 'final' and e['text']]
    clips = schedule['clips']
    references = {p.stem: json.loads(p.read_text('utf-8')) for p in (ROOT / 'regression/reference').glob('*.json')}
    # The pipeline timeline starts when capture starts; playback started about a second later. Estimate the
    # offset from the first final (its start includes 0.3 s pre-roll and any leading silence of the clip), then
    # assign every final to the clip it overlaps most.
    offset = None
    if finals and clips:
        offset = finals[0]['audio_start'] - clips[0]['start']
        # Refine: median over finals of the distance to the nearest clip start under the first estimate.
        deltas = []
        for f in finals:
            nearest = min(clips, key=lambda c: abs(c['start'] - (f['audio_start'] - offset)))
            deltas.append(f['audio_start'] - nearest['start'])
        deltas.sort(); offset = deltas[len(deltas) // 2]
    assigned = {clip['id']: [] for clip in clips}
    for f in finals:
        if offset is None: break
        best, best_overlap = None, 0.0
        for clip in clips:
            overlap = min(f['audio_end'] - offset, clip['end']) - max(f['audio_start'] - offset, clip['start'])
            if overlap > best_overlap: best, best_overlap = clip['id'], overlap
        if best: assigned[best].append(f)
    scored = []
    for clip in clips:
        matches = assigned[clip['id']]
        hypothesis = ' '.join(f['text'] for f in matches)
        characters = clip['category'] == 'mandarin'
        ref_h, hyp = tokens(clip['human_text'], characters), tokens(hypothesis, characters)
        ref_o = tokens(references.get(clip['id'], {}).get('text', ''), characters)
        scored.append({'id': clip['id'], 'category': clip['category'], 'finals': len(matches), 'hypothesis': hypothesis,
                       'human_edits': edits(ref_h, hyp), 'human_units': len(ref_h), 'oracle_edits': edits(ref_o, hyp), 'oracle_units': len(ref_o)})
    summary = {}
    for cat in ['mandarin', 'english', 'mixed']:
        rows = [s for s in scored if s['category'] == cat]
        if rows:
            summary[cat] = {'clips': len(rows), 'human_error_rate': sum(r['human_edits'] for r in rows) / max(1, sum(r['human_units'] for r in rows)),
                            'oracle_error_rate': sum(r['oracle_edits'] for r in rows) / max(1, sum(r['oracle_units'] for r in rows)),
                            'clips_without_caption': sum(1 for r in rows if not r['finals'])}
    result = {'name': args.name, 'playlist': args.playlist, 'device': body['source'].get('device'), 'background_load_processes': args.load,
              'capture_started': started_line, 'alignment_offset_seconds': offset, 'clips': scored, 'summary': summary,
              'live': {k: v for k, v in body['live'].items() if k not in ('events', 'utterances', 'transcript')}, 'success': body.get('success')}
    (ROOT / 'reports' / f'capture-{args.name}-scored.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'summary': summary, 'success': body.get('success'), 'stop_reason': body['live']['pipeline'].get('stop_reason'),
                      'provisional_lag_p95': body['live']['provisional_lag_seconds']['p95'], 'first_caption_p95': body['live']['first_caption_lag_seconds']['p95'],
                      'finalization_p95': body['live']['finalization_delay_seconds']['p95'], 'max_backlog': body['live']['pipeline']['max_backlog_seconds']}, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
