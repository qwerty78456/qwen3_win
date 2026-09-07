"""Drive the interface through Win32 messages while a player renders fixture audio.

Starts AsrWin.exe without arguments, waits for the model to load, presses Start, plays the
first part of the fixture playlist through the default endpoint with ffplay, records when the
first provisional and final captions appear in the controls, presses Stop, reads the final
transcript, closes the window and checks the exit code. Writes reports/gui-test.json.
Build-time only; needs an interactive desktop session on the development host.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import subprocess
import time
import unicodedata
from bootstrap import ROOT
from shipped import MODEL_DIR

user32 = ctypes.windll.user32
IDC_DEVICE, IDC_REFRESH, IDC_START, IDC_STOP, IDC_SAVE, IDC_REPORT, IDC_NOTICE, IDC_STATUS, IDC_PROVISIONAL, IDC_TRANSCRIPT = range(101, 111)
WM_CLOSE, WM_COMMAND, WM_GETTEXT, WM_GETTEXTLENGTH, BN_CLICKED, CB_GETCOUNT = 0x10, 0x111, 0x0D, 0x0E, 0, 0x146
EnumChildProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

def children(window):
    found = {}
    def callback(h, _):
        found[user32.GetDlgCtrlID(h)] = h
        return True
    user32.EnumChildWindows(window, EnumChildProc(callback), 0)
    return found

def text(h):
    n = user32.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    buffer = ctypes.create_unicode_buffer(n + 1)
    user32.SendMessageW(h, WM_GETTEXT, n + 1, buffer)
    return buffer.value

def normalize(s):
    s = unicodedata.normalize('NFKC', s).lower()
    return ''.join(ch for ch in s if ch.isalnum() or 0x3400 <= ord(ch) <= 0x9fff)

def tokens(text, characters):
    text = unicodedata.normalize('NFKC', text)
    out, word = [], ''
    for ch in text:
        cp = ord(ch)
        if 0x3400 <= cp <= 0x9fff or 0xf900 <= cp <= 0xfaff:
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

def main():
    import ctypes
    # Keep the display and its HDMI/DP audio endpoint awake for the duration of the test (per-process request, no system setting change).
    ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000002 | 0x00000001)
    parser = argparse.ArgumentParser()
    parser.add_argument('--playlist', default='playlist-1x')
    parser.add_argument('--seconds', type=float, default=60, help='playback length')
    args = parser.parse_args()
    exe = ROOT / 'build/Release/AsrWin.exe'
    schedule = json.loads((ROOT / 'traces/live' / f'{args.playlist}.json').read_text('utf-8'))
    references = {p.stem: json.loads(p.read_text('utf-8'))['text'] for p in (ROOT / 'regression/reference').glob('*.json')}
    result = {'success': False, 'steps': []}
    def step(name, ok, **detail):
        result['steps'].append({'step': name, 'success': bool(ok), **detail}); print(name, 'OK' if ok else 'FAIL', detail, flush=True)
        return ok
    process = subprocess.Popen([str(exe), '--model', str(MODEL_DIR), '--gui'], creationflags=subprocess.CREATE_NEW_CONSOLE)
    window = 0
    deadline = time.time() + 30
    while time.time() < deadline and not window:
        window = user32.FindWindowW('AsrWinMain', None); time.sleep(0.2)
    if not step('window appears', window): process.kill(); raise SystemExit(1)
    controls = children(window)
    step('controls present', all(k in controls for k in (IDC_DEVICE, IDC_START, IDC_STOP, IDC_SAVE, IDC_STATUS, IDC_PROVISIONAL, IDC_TRANSCRIPT)),
         notice=text(controls[IDC_NOTICE]), devices=user32.SendMessageW(controls[IDC_DEVICE], CB_GETCOUNT, 0, 0))
    step('notice text', text(controls[IDC_NOTICE]) == 'Captures computer playback audio. Does not record the microphone.')
    step('start disabled while loading', not user32.IsWindowEnabled(controls[IDC_START]), status=text(controls[IDC_STATUS]))
    t0 = time.time(); status = ''
    while time.time() - t0 < 180:
        status = text(controls[IDC_STATUS])
        if status.startswith('Ready') or status.startswith('Error'): break
        time.sleep(0.5)
    load_seconds = time.time() - t0
    if not step('model loads', status.startswith('Ready'), status=status, load_seconds=round(load_seconds, 1)): user32.PostMessageW(window, WM_CLOSE, 0, 0); process.wait(30); raise SystemExit(1)
    step('start enabled after load', user32.IsWindowEnabled(controls[IDC_START]))
    user32.PostMessageW(window, WM_COMMAND, (BN_CLICKED << 16) | IDC_START, controls[IDC_START])
    time.sleep(1.5)
    status = text(controls[IDC_STATUS])
    step('listening after start', status.startswith('Listening'), status=status, stop_enabled=bool(user32.IsWindowEnabled(controls[IDC_STOP])), start_enabled=bool(user32.IsWindowEnabled(controls[IDC_START])))
    play_started = time.time()
    player = subprocess.Popen(['ffplay', '-nodisp', '-autoexit', '-loglevel', 'quiet', '-t', str(args.seconds), str(ROOT / 'traces/live' / f'{args.playlist}.wav')])
    first_provisional = first_final = None; provisional_samples = []; statuses = set()
    while player.poll() is None or time.time() - play_started < args.seconds + 6:
        p = text(controls[IDC_PROVISIONAL]); tr = text(controls[IDC_TRANSCRIPT]); statuses.add(text(controls[IDC_STATUS]).split(':')[0])
        if p and first_provisional is None: first_provisional = time.time() - play_started
        if p and (not provisional_samples or provisional_samples[-1] != p): provisional_samples.append(p)
        if tr.strip() and first_final is None: first_final = time.time() - play_started
        time.sleep(0.25)
        if time.time() - play_started > args.seconds + 20: break
    player.wait()
    user32.PostMessageW(window, WM_COMMAND, (BN_CLICKED << 16) | IDC_STOP, controls[IDC_STOP])
    t1 = time.time(); status = ''
    while time.time() - t1 < 120:
        status = text(controls[IDC_STATUS])
        if status.startswith('Stopped'): break
        time.sleep(0.25)
    stop_seconds = time.time() - t1
    transcript = text(controls[IDC_TRANSCRIPT])
    step('first provisional caption appeared', first_provisional is not None, seconds_after_playback_start=None if first_provisional is None else round(first_provisional, 2),
         first_clip_start=schedule['clips'][0]['start'], provisional_samples=provisional_samples[:6])
    step('final captions appeared', bool(transcript.strip()), seconds_after_playback_start=None if first_final is None else round(first_final, 2))
    step('stopped after Stop', status.startswith('Stopped'), status=status, stop_seconds=round(stop_seconds, 2), statuses_seen=sorted(statuses))
    step('provisional area cleared after stop', text(controls[IDC_PROVISIONAL]) == '')
    step('start enabled again', bool(user32.IsWindowEnabled(controls[IDC_START])))
    expected = [c for c in schedule['clips'] if c['end'] <= args.seconds]
    partial = any(c['start'] < args.seconds < c['end'] for c in schedule['clips'])
    lines = [l for l in transcript.replace('\r', '').split('\n') if l.strip()]
    scored_lines = lines[:-1] if partial and len(lines) > len(expected) else lines  # drop the caption of a clip cut by the playback limit
    exact = sum(1 for c in expected if any(normalize(references.get(c['id'], '')) == normalize(l) for l in lines))
    ref_tokens = [t for c in expected for t in tokens(references.get(c['id'], ''), True)]
    hyp_tokens = [t for l in scored_lines for t in tokens(l, True)]
    rate = edits(ref_tokens, hyp_tokens) / max(1, len(ref_tokens))
    # Utterances may be split at pauses differently from the file oracle, so score the pooled text (character units).
    step('finals match oracle text for fully played clips', rate <= 0.2, pooled_error_rate=round(rate, 3), exact_line_matches=exact, expected=len(expected), lines=lines)
    step('save enabled with transcript', bool(user32.IsWindowEnabled(controls[IDC_SAVE])))
    user32.PostMessageW(window, WM_CLOSE, 0, 0)
    try: code = process.wait(60)
    except subprocess.TimeoutExpired: process.kill(); code = None
    step('window closes cleanly', code == 0, exit_code=code)
    result['success'] = all(s['success'] for s in result['steps'])
    result['transcript'] = transcript
    (ROOT / 'reports/gui-test.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('gui-test', 'PASS' if result['success'] else 'FAIL')
    raise SystemExit(0 if result['success'] else 1)

if __name__ == '__main__':
    main()
