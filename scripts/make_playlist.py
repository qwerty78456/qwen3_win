"""Build the playback material for loopback tests: every speech fixture with pauses, repeated.

Writes traces/live/playlist.wav (16 kHz mono PCM16) and traces/live/playlist.json with the
expected order and timing, so a capture run can be scored against the fixture transcripts.
"""
import argparse
import json
import numpy as np
import soundfile as sf
from bootstrap import ROOT

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--gap', type=float, default=1.5, help='silence between clips, seconds')
    parser.add_argument('--lead', type=float, default=2.0)
    parser.add_argument('--name', default='playlist')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'regression/manifest.json').read_text('utf-8'))
    out = ROOT / 'traces/live'
    out.mkdir(parents=True, exist_ok=True)
    pieces = [np.zeros(int(args.lead * 16000), np.float32)]
    schedule = []
    position = args.lead
    for _ in range(args.repeats):
        for fixture in manifest['fixtures']:
            if fixture['category'] == 'non-speech':
                continue
            audio, rate = sf.read(ROOT / 'regression' / fixture['path'], dtype='float32', always_2d=True)
            mono = audio.mean(axis=1, dtype=np.float32)
            if rate != 16000:
                import librosa
                mono = librosa.resample(mono, orig_sr=rate, target_sr=16000).astype(np.float32)
            schedule.append({'id': fixture['id'], 'category': fixture['category'], 'start': position, 'end': position + len(mono) / 16000, 'human_text': fixture['human_text']})
            pieces.append(mono)
            pieces.append(np.zeros(int(args.gap * 16000), np.float32))
            position += len(mono) / 16000 + args.gap
    playlist = np.concatenate(pieces)
    sf.write(out / f'{args.name}.wav', playlist, 16000, subtype='PCM_16')
    (out / f'{args.name}.json').write_text(json.dumps({'duration_seconds': len(playlist) / 16000, 'clips': schedule}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'{args.name}.wav: {len(playlist) / 16000:.1f} s, {len(schedule)} clips')

if __name__ == '__main__':
    main()
