# AsrWin — offline Mandarin / English playback captions (test candidate)

AsrWin captions the audio your computer plays back. Captions stay in the spoken language, including switches between Mandarin and English within one sentence. Nothing is translated and nothing leaves the machine.

**Captures computer playback audio. Does not record the microphone.**

## Status

This is a **test candidate**. Transcription correctness of the bundled model was proved against the official Qwen3-ASR model on the 24-fixture regression set on the reference PC, and live captioning was measured on that PC only. Clean-Windows installation and sustained real-time behaviour on other machines have **not** been verified yet; see `docs/COMPATIBILITY.md` for what was actually tested and `docs/BENCHMARK.md` for the measured latency.

## Requirements

- Windows 10 22H2 x64 or Windows 11 x64 with Media Foundation (Windows N editions without the Media Feature Pack are not supported).
- About 15 GB of free disk space after extraction (the folder carries the shipped 0.6B model and the optional GPU-only 1.7B model) and 8 GB of RAM: the shipped Qwen3-ASR-0.6B FP32 model peaks at about 6.3 GiB of working set while listening (`docs/BENCHMARK.md`). `shipped-model.json` names the exact configuration; the 1.7B evidence and the first DirectML probe are described in `docs/RESUME-AUDIT.md`.
- A 64-bit CPU with 8 or more threads is recommended. No GPU is required: the CPU is the default. A DirectX 12 GPU with enough dedicated memory (about 5.7 GB for the shipped model, about 14 GB for the 1.7B model) can be chosen in the interface's compute list or with `--provider directml` when this build marks DirectML as validated; `docs/COMPATIBILITY.md` names the adapter and driver it was validated on and everything else is untested. The 1.7B model also needs about 24 GB of RAM. CPU operation always remains available.

## Use

1. Extract the complete folder and start `AsrWin.exe`. Package and model hashes are checked during loading (a few seconds). Paths containing spaces or Chinese characters are part of the documented test checklist (`docs/CLEAN-WINDOWS-TEST.md`); external clean-machine verification is still pending.
2. Choose "Default playback device" or a specific speaker/headphone output. Optionally pick a GPU in the compute list below it (`GPU: <adapter> — Qwen3-ASR-0.6B`, or `… — Qwen3-ASR-1.7B (higher accuracy)` on adapters with about 14 GB or more); the model is reloaded for the new choice and Start is enabled again when it is ready. If the GPU cannot be used, the status line and a message box say why — select CPU again to continue. Then press **Start**. The default device is resolved when you press Start; if the default changes or the device is removed, listening stops and the audio already captured is transcribed.
3. Provisional captions appear in the upper box while a sentence is still being spoken (measured lag on the reference PC is in `docs/BENCHMARK.md`). Finished sentences move to the scrollable transcript below. A "Catching up" status means transcription is behind the audio and provisional captions are paused until it recovers.
4. **Stop** finishes the sentence in progress. **Save…** writes the transcript as UTF-8 text with a byte-order mark. **Report…** writes a diagnostic JSON file with timing, memory and compute-selection statistics (no audio, no text).

Audio is kept only in memory and is released as soon as it has been transcribed. Transcripts are written only when you press Save. The compute choice is remembered in `%LOCALAPPDATA%\AsrWin\settings.json` (delete it to return to the CPU default); nothing else is written outside the folder.

## Command line

```text
AsrWin.exe --benchmark input.wav --provider cpu --report results.json
AsrWin.exe --benchmark input.wav --provider directml --adapter default --report results-gpu.json
AsrWin.exe --benchmark input.wav --simulate-live --report live.json
AsrWin.exe --verify regression\manifest.json --report verification.json
AsrWin.exe --verify regression\manifest-1.7b.json --large-model --provider directml --report verification-1.7b.json
AsrWin.exe --capture --device default --duration 60 --report live.json
AsrWin.exe --adapters
AsrWin.exe --check-package
AsrWin.exe --diagnostics
```

WAV input must be integer PCM (8/16/24/32-bit) or 32-bit float. Nonzero exit codes indicate invalid input, inference failure or failed verification.

## Contents

- `models/` — the shipped Qwen3-ASR-0.6B ONNX model and the optional GPU-only Qwen3-ASR-1.7B model (see `shipped-model.json` and each model folder's `manifest.json` for the exact configuration and asset hashes).
- `regression/` — the bilingual regression set (FLEURS and ASCEND excerpts plus generated non-speech), human transcripts and official-model references for both models (`manifest.json`, `manifest-1.7b.json`).
- `licenses/`, `THIRD_PARTY_NOTICES.md` — notices for every bundled component and dataset attribution.
- `docs/` — proof report, benchmark report, compatibility matrix and packaging instructions.
