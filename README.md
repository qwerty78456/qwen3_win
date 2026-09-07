# AsrWin

Offline Mandarin/English playback captions for Windows x64. One C++20 executable (`AsrWin.exe`) contains the Win32 interface, WASAPI loopback capture, preprocessing, speech gating, Qwen3-ASR-1.7B ONNX inference, the file benchmark, regression verification and diagnostics. No helper processes, no Python at runtime, no downloads.

**Development status: test candidate.** The transcription proof, boundary/session tests, CPU thread budget, memory configuration, simulated live runs and real loopback captures were completed on the reference PC (see `docs/PROOF.md`, `docs/BENCHMARK.md`). Clean-Windows verification on external machines and the sustained-live release gate have **not** been passed; `docs/COMPATIBILITY.md` lists exactly what was tested. No clean-install or real-time support claim is made.

Capture is playback-device loopback, not microphone recording. Mandarin and English remain in the language spoken; this application does not translate.

## Gates

| Gate | State | Evidence |
|---|---|---|
| 1. Complete transcription proof | done on the reference PC | `docs/PROOF.md`, `reports/verification.json`, `reports/stages/`, `reports/boundaries.json` |
| 2. One executable, portable package | built; sizes measured | `scripts/package.py`, `reports/package.json`, `docs/PACKAGING.md` |
| 3. Capture, scheduling, interface | implemented; automated interface and loopback tests on the reference PC | `src/capture.cpp`, `src/pipeline.cpp`, `src/gui.cpp`, `reports/gui-test.json`, `reports/capture-*.json` |
| 4. Correctness and performance gates | CPU measured; DirectML not enabled | `reports/benchmark-threads.json`, `reports/memory.json`, `reports/live-*.json`, `docs/BENCHMARK.md` |
| 5. Clean Windows verification and release | **pending external machines** | `docs/COMPATIBILITY.md` |

## Layout

- `src/` — application sources (`main.cpp` modes, `asr.cpp` runner, `mel.cpp` features, `audio.cpp` WAV/resampler, `segmenter.cpp` speech gate, `capture.cpp` WASAPI loopback, `pipeline.cpp` scheduler, `gui.cpp` interface, `scoring.cpp` CER/WER/MER).
- `scripts/` — build-time acquisition, reference oracle, tests, benchmarks, packaging and report generation (`docs/BUILD.md`).
- `regression/` — 24 frozen fixtures with human transcripts and official-model references.
- `reports/` — measured JSON reports; `docs/` — generated proof, benchmark and compatibility documents.
- Lock files: `dependencies.lock.json` (sources, model assets, hashes), `remote-assets.lock.json` (publisher hashes), `native-dependency.lock.json` (DirectML), `tokenizer.Cargo.lock`, `reference-environment.lock.txt`, `verification-policy.json`.

## Model

The shipped configuration is recorded in `shipped-model.json`: Qwen3-ASR-0.6B FP32 ONNX graphs (andrewleech/qwen3-asr-0.6b-onnx, pinned revision) with FP16 embedding storage, ONNX Runtime 1.24.4 (Microsoft.ML.OnnxRuntime.DirectML native x64, CPU provider). Development started with the 1.7B FP32 export, whose complete proof is archived under `reports/proof-1.7b/`; because the 1.7B decoder missed the provisional-latency target and peaked at 15.25 GiB, the plan's smaller-configuration rule was applied on a held-out set (`evaluation/`, `reports/variant-evaluation.json`): 1.7B int4 was rejected (+2.0 pp Mandarin CER) and 0.6B FP32 accepted (no category worse than the 1.7B baseline, no new silence failures). The encoder uses the model's designed 8-second attention windows; `docs/PROOF.md` §2 documents the difference from the pinned official eager code path and how the oracle accounts for it.
