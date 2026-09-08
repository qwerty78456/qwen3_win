# AsrWin — offline Mandarin / English playback captions (test candidate)

AsrWin captions the audio your computer plays back. Captions stay in the spoken language, including switches between Mandarin and English within one sentence. Nothing is translated and nothing leaves the machine.

**Captures computer playback audio. Does not record the microphone.**

## Status

This is a **test candidate**. Transcription correctness of the bundled model was proved against the official Qwen3-ASR model on the 24-fixture regression set on the reference PC, and live captioning was measured on that PC only. Clean-Windows installation and sustained real-time behaviour on other machines have **not** been verified yet; see `docs/COMPATIBILITY.md` for what was actually tested and `docs/BENCHMARK.md` for the measured latency.

## Requirements

- Windows 10 22H2 x64 or Windows 11 x64 with Media Foundation (Windows N editions without the Media Feature Pack are not supported).
- About 5 GB of free disk space after extraction and 8 GB of RAM: the shipped Qwen3-ASR-0.6B FP32 model peaks at about 6.3 GiB of working set while listening (`docs/BENCHMARK.md`). `shipped-model.json` names the exact configuration; the 1.7B evidence and the DirectML probe are described in `docs/RESUME-AUDIT.md`.
- A 64-bit CPU with 8 or more threads is recommended. No GPU is required or used; DirectML is disabled in the interface and only reachable through explicit experimental command-line flags.

## Use

1. Extract the complete folder and start `AsrWin.exe`. Package and model hashes are checked during loading (a few seconds). Paths containing spaces or Chinese characters are part of the documented test checklist (`docs/CLEAN-WINDOWS-TEST.md`); external clean-machine verification is still pending.
2. Choose "Default playback device" or a specific speaker/headphone output, then press **Start**. The default device is resolved when you press Start; if the default changes or the device is removed, listening stops and the audio already captured is transcribed.
3. Provisional captions appear in the upper box while a sentence is still being spoken (measured lag on the reference PC is in `docs/BENCHMARK.md`). Finished sentences move to the scrollable transcript below. A "Catching up" status means transcription is behind the audio and provisional captions are paused until it recovers.
4. **Stop** finishes the sentence in progress. **Save…** writes the transcript as UTF-8 text with a byte-order mark. **Report…** writes a diagnostic JSON file with timing and memory statistics (no audio).

Audio is kept only in memory and is released as soon as it has been transcribed. Transcripts are written only when you press Save.

## Command line

```text
AsrWin.exe --benchmark input.wav --provider cpu --report results.json
AsrWin.exe --benchmark input.wav --simulate-live --report live.json
AsrWin.exe --verify regression\manifest.json --report verification.json
AsrWin.exe --capture --device default --duration 60 --report live.json
AsrWin.exe --check-package
AsrWin.exe --diagnostics
```

WAV input must be integer PCM (8/16/24/32-bit) or 32-bit float. Nonzero exit codes indicate invalid input, inference failure or failed verification.

## Contents

- `models/` — the shipped Qwen3-ASR ONNX model (see `shipped-model.json` and the model folder's `manifest.json` for the exact configuration and asset hashes).
- `regression/` — the bilingual regression set (FLEURS and ASCEND excerpts plus generated non-speech), human transcripts and official-model references.
- `licenses/`, `THIRD_PARTY_NOTICES.md` — notices for every bundled component and dataset attribution.
- `docs/` — proof report, benchmark report, compatibility matrix and packaging instructions.
