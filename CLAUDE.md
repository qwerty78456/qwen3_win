# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AsrWin: one C++20 Windows x64 executable (`AsrWin.exe`) that captions computer *playback* audio (WASAPI loopback, never the microphone) in Mandarin/English using a pinned Qwen3-ASR ONNX export on the CPU. No runtime Python, no network code, no installer. The same executable is the GUI, the CLI benchmark/verification tool, the self-test and the package checker. Status is "test candidate": gates 1–4 are proven on the reference PC, gate 5 (clean external Windows machines) is pending. `README.md` is the authoritative user/deployment guide; `docs/BUILD.md` is the full developer command sequence.

## Two Python interpreters (important)

- **System `python` (3.12)**: acquisition, manifests, packaging — `bootstrap.py`, `native_deps.py`, `acquire_0_6b.py`, `model_manifest.py`, `check_publisher_hashes.py`, `collect_licenses.py`, `package.py`.
- **`.venv\Scripts\python.exe`**: anything that imports torch/numpy/soundfile/transformers — `reference.py`, every `test_*.py`, `compare_traces.py`, `proof_report.py`, `evaluate_variants.py`, `gui_test.py`, `live_capture_test.py`. Versions are frozen in `reference-environment.lock.txt`. `package.py` itself shells out to the venv for the docs refresh.

Scripts do `from bootstrap import ROOT` / `from shipped import ...`, so invoke them by path (`python scripts/x.py`) so `scripts/` lands on `sys.path`. Run from the repo root.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1     # -> build\Release\AsrWin.exe
```

`build.ps1` locates VS Build Tools 2026 via vswhere, pins the VC 14.51.36231 x64 CRT redist, configures CMake ("Visual Studio 18 2026", x64) into `build/` and builds Release with 4 parallel jobs. CMake fails fast if `.deps/` is missing (run `python scripts/bootstrap.py` then `python scripts/native_deps.py` once; both are resumable — never delete `*.part` files). The tokenizer is a Rust static lib built by Cargo inside the CMake build (`tokenizer.Cargo.lock` is copied in; `CARGO_HOME=.cache/cargo`).

`ASRWIN_MODEL_DIR`, `ASRWIN_THREADS`, `ASRWIN_DEFAULT_PROVIDER`, `ASRWIN_DIRECTML_VALIDATED`, `ASRWIN_LARGE_MODEL_DIR` and `ASRWIN_LARGE_MODEL_THREADS` are compiled in from `shipped-model.json` (CMake `string(JSON)`; `directml.validated`, `directml.large_model.*`); the Python side reads the same file through `scripts/shipped.py` (`MODELS['default'|'large']`, `model_config(key)`, `DIRECTML_VALIDATED`). **Changing the shipped model/threads/provider policy = edit `shipped-model.json`, rebuild, re-run `model_manifest.py` (and `--model-key large`), the proof scripts, and `package.py`.** Current: Qwen3-ASR-0.6B FP32, CPU default, 4 threads, plus Qwen3-ASR-1.7B FP32 as the GPU-only large model (`--large-model`, 6 threads); the archived 1.7B CPU evidence is under `reports/proof-1.7b/` and `reports/current-1.7b/`.

LNK1104 on relink means an `AsrWin.exe` (a test, the GUI, a capture) is still running — stop it, don't fight the build.

## Running and testing

There is no pytest/ctest. Every check is either an executable mode or a standalone script that writes a JSON report under `reports/` and exits nonzero on failure. Exit codes from `AsrWin.exe`: 0 ok, 1 invalid input/failure, 2 ran but did not pass.

**Dev builds must always pass `--model`.** Without it the engine calls `verify_package()` on the executable's directory, which needs a `package-manifest.json` — only a packaged folder has one. The model directory is the `model_dir` in `shipped-model.json`.

```powershell
# native regression: 24 frozen fixtures vs official-model references (tokens must be identical)
build\Release\AsrWin.exe --verify regression\manifest.json --model models\qwen3-asr-0.6b-fp32 --report reports\verification.json --trace traces\native

# one fixture end to end, then numerical stage comparison against the oracle traces
build\Release\AsrWin.exe --benchmark regression\mandarin-0000.wav --model models\qwen3-asr-0.6b-fp32 --trace traces\native\mandarin-0000 --report out.json
.venv\Scripts\python.exe scripts/compare_traces.py mandarin-0000 --report reports\stages\mandarin-0000.json

# native queue/lifecycle/Unicode tests with a synthetic decoder (no model needed)
build\Release\AsrWin.exe --self-test regression\mandarin-0003.wav

# real-time replay through the live pipeline / real loopback capture
build\Release\AsrWin.exe --benchmark regression\english-0002.wav --simulate-live --model models\qwen3-asr-0.6b-fp32 --report reports\live-english-0002.json
build\Release\AsrWin.exe --capture --device default --duration 60 --model models\qwen3-asr-0.6b-fp32 --report capture.json

# DirectML (GPU): list adapters, run on one; on an unvalidated build add --experimental-directml and an explicit --adapter N
build\Release\AsrWin.exe --adapters --model models\qwen3-asr-0.6b-fp32 --large-model-dir models\qwen3-asr-1.7b-fp32
build\Release\AsrWin.exe --verify regression\manifest.json --model models\qwen3-asr-0.6b-fp32 --provider directml --adapter 0 --experimental-directml --trace traces\native-directml --report reports\verification-directml.json
build\Release\AsrWin.exe --verify regression\manifest-1.7b.json --model models\qwen3-asr-1.7b-fp32 --large-model --provider directml --adapter 0 --experimental-directml --report reports\verification-directml-1.7b.json
.venv\Scripts\python.exe scripts/compare_traces.py mandarin-0000 --provider directml [--model-key large]   # reports\stages-directml[-1.7b]\
.venv\Scripts\python.exe scripts/directml_proof.py --adapter default --models default,large   # the whole DirectML gate -> reports\directml.json

# Python-driven checks (each is self-contained; see docs/BUILD.md for the full ordered list)
.venv\Scripts\python.exe scripts/test_preprocessing.py        # native mel/resampling vs official processor
.venv\Scripts\python.exe scripts/test_speech_gate.py          # segmenter on synthetic audio
.venv\Scripts\python.exe scripts/test_cli_failures.py         # invalid inputs / missing assets / adapter validation through the exe
.venv\Scripts\python.exe scripts/test_validator_failures.py   # proves compare_traces.py rejects corruption (no model)
.venv\Scripts\python.exe scripts/test_boundaries.py           # 8 s window edges, 15 s limit, repeated sessions (needs oracle)
.venv\Scripts\python.exe scripts/gui_test.py --seconds 60 [--provider directml [--large-model]] [--corrupt-settings]   # Win32-message-driven interface test (plays audio; settings redirected to traces/gui-test)
.venv\Scripts\python.exe scripts/live_capture_test.py --playlist playlist-1x --name 3min-load2 --load 2 [--provider directml --adapter default [--large-model]]
```

Regenerating oracle references (`.venv\Scripts\python.exe scripts/reference.py --threads 8`) runs the official PyTorch model and is slow; `--fixture <id>` limits it, `--extra x.wav` handles ad-hoc files, `--manifest evaluation/manifest.json` targets the held-out set, `--model-key large` targets the 1.7B model (`regression/reference-1.7b`, `traces/reference-1.7b`). `scripts/prepare_large_model_references.py` installs the archived 1.7B references and writes `regression/manifest-1.7b.json` / `evaluation/manifest-1.7b.json` without running the oracle.

## Packaging

```powershell
.venv\Scripts\python.exe scripts/reference.py --prepare-only   # mel_filters.bin, prompt_reference.json into the model folder
python scripts/model_manifest.py                               # freeze asset hashes (fails on incomplete downloads)
python scripts/model_manifest.py --model-key large             # the GPU-only 1.7B model (package.py ships both)
python scripts/package.py --version 0.1.0-tcN --replace        # dist\AsrWin-<v>-win-x64[.zip] + release-info.json
cd dist\AsrWin-0.1.0-tcN-win-x64 ; .\AsrWin.exe --check-package
```

`package.py` regenerates `docs/PROOF.md`, `docs/BENCHMARK.md`, `docs/COMPATIBILITY.md` from `reports/` before hashing (skip with `--no-docs-refresh`). **Those three docs are generated by `scripts/proof_report.py` — never hand-edit them; fix the report or the generator.** `docs/PACKAGE-README.md` becomes the package's `README.md`.

## Architecture

**Executable (`src/`, ~1.7k lines, dense one-statement-per-line style, `/W4 /permissive- /utf-8`, UNICODE Win32, nlohmann JSON everywhere).**

- `main.cpp` — `wmain` parses flags, enforces *exactly one action per invocation*, and dispatches. No arguments → GUI (console freed when launched from Explorer). Contains `LiveCollector`, which turns pipeline events into the `--simulate-live`/`--capture` report and computes the success criteria (no errors, no incomplete/duplicate finals, no stale provisionals, clean stop, no audio loss).
- `asr.cpp` — `Engine`: three ONNX Runtime sessions (`encoder.onnx`, `decoder_init*.onnx` prefill, `decoder_step*.onnx` cached step; the two decoders share one `PrepackedWeightsContainer` in the default `shared-prepack` memory mode), memory-mapped `embed_tokens.bin` (FP16), tokenizers-cpp with a startup self-check against `prompt_reference.json`. Greedy decode, KV cache handed back each step, shapes asserted every step, optional `--trace` dumps of `pcm/mel/encoder/input_embeds/logits_*/keys_*/values_*` at steps 0, 1, 7. On construction it verifies the package (unless `--model`) and the model manifest (`verify_files`). With `EngineOptions::directml` it appends the DML provider for the DXGI adapter index, then confirms through `OrtDmlApi::GetDMLDevice` that the bound device's LUID is the enumerated adapter's (`adapter_luid_confirmed`); `warm_up()` runs one short generation (live paths only) and `gpu_memory()`/`detail["gpu_memory"]` record `IDXGIAdapter3::QueryVideoMemoryInfo`.
- `compute.cpp` — DXGI adapter enumeration (`compute_adapters`, `--adapters`), the eligibility rule (hardware only, one per LUID, dedicated memory ≥ 1.5 × the model's graph bytes from `manifest.json`), `resolve_adapter("default"|index)` with the exact refusal messages that `test_cli_failures.py` asserts. `settings.cpp` — the interface's `%LOCALAPPDATA%\AsrWin\settings.json` (provider, adapter LUID, large model), written only after a successful load, never read by the CLI.
- Front end: `audio.cpp` (WAV reader, Media Foundation resampler → 16 kHz mono), `mel.cpp` (128-bin log-mel via kissfft; pads input to ≥ 1.0 s so the ONNX graph's conv chunking matches the official code), `segmenter.cpp` (libfvad mode 2, 20 ms frames, −80 dBFS floor, 100 ms open / 700 ms close / 300 ms pre-roll / hard 15 s limit).
- Live path: `capture.cpp` `LoopbackCapture` (event-driven WASAPI shared-mode loopback plus a silent render stream to keep packets flowing; the capture thread only copies raw bytes + QPC timestamps + flags into the lock-free `RawRing`) or `pipeline.cpp` `FileSource` (paced WAV replay through the same ring) → `LivePipeline`: a **preprocess thread** (mono, resample, segment, schedule provisionals) and **one inference worker** (finals always win: a queued final sets `cancel_provisional_`, and provisional results for finished/superseded utterances are dropped). Backlog = age of the oldest unfinished utterance: > 4 s suspends provisionals, < 2 s resumes, > 20 s stops the session; ≤ 60 s of audio retained. Everything reaches consumers as `CaptionEvent` (Status/Provisional/Final/Stopped/Error) through a single sink.
- `gui.cpp` — plain Win32 window; loads the engine on a background thread and marshals events to the UI thread with `WM_APP_*` messages. The compute combo (`IDC_COMPUTE`, appended last — `gui_test.py` mirrors control IDs positionally, so never insert IDs in the middle) lists CPU, then each eligible adapter for the default model and, when it has the memory, the large model; GPU items appear only in validated builds or with `--experimental-directml`. Changing it calls `start_loading()`, which resets the `LivePipeline` **before** the `Engine` (the pipeline holds an `Engine&`), destroys the old engine on the loader thread and reloads; a failed GPU load leaves the engine empty, shows the error and lets the user pick CPU (no automatic fallback). Initial choice: command line > settings file (by LUID) > compiled default. `self_test.cpp` — ring/pipeline/segmenter tests using a synthetic decoder. `scoring.cpp` — CER/WER/MER and the normalization in `verification-policy.json`.

**Proof toolchain (`scripts/`).** `reference.py` is the independent oracle: it runs the *unmodified* official Qwen3-ASR processor/model from `reference-model-0.6b/` (or `reference-model/` with `--model-key large`) and writes `regression/reference[-1.7b]/<id>.json` plus `traces/reference[-1.7b]/<id>/*.f32`. It applies the encoder 8 s window mask that the pinned eager code path omits (`docs/PROOF.md` §2; `--global-attention` reproduces the unmasked path). The native `--trace` output lands in `traces/native[-directml][-1.7b]/<id>/`; `compare_traces.py [--provider directml] [--model-key large]` compares stage by stage against the tolerances in `verification-policy.json` (element-wise, with a tensor-scale alternative for keys/values; an optional `stages.<name>.providers.directml` override) and also demands identical prompt, tokens, cache shapes and EOS completion, writing `reports/stages[-directml][-1.7b]/`. Any tolerance change must be recorded in `verification-policy.json`'s `tolerance_changes` with a reason.

**Data sets.** `regression/` is the frozen 24-fixture set (6 mandarin / 6 english / 6 mixed / 6 non-speech; `--verify` refuses anything else and checks every fixture and reference hash). `evaluation/` is the disjoint held-out set used *only* for the configuration decision (`evaluate_variants.py` → `reports/variant-evaluation.json`); never tune on `regression/`. `acquire_fixtures.py` refuses to alter a frozen selection.

**Pinning.** `dependencies.lock.json` (source revisions, ORT package, model files + hashes), `remote-assets.lock.json` (publisher hashes), `native-dependency.lock.json` (DirectML), `tokenizer.Cargo.lock`, `reference-environment.lock.txt`, and per-model `manifest.json`. Untracked-by-design: `models/`, `reference-model*/`, `.deps/`, `.venv/`, `.cache/`, `build/`, `dist/`, `traces/`.

## Conventions and constraints

- Every measured number in the docs comes from a JSON report in `reports/`; `proof_report.py` prints "not measured" rather than inventing values. Keep it that way.
- Reports and the `Report…` button must never contain audio or transcript text; transcripts are written only on explicit Save/`--transcript` (UTF-8 with BOM).
- DirectML is gated by `shipped-model.json` → `directml.validated`. While false: the CLI rejects `--provider directml` without `--experimental-directml` *and* an explicit `--adapter N`, and the GUI shows GPU items only with `--experimental-directml`. Flip it to true **only** from a passing `reports/directml.json` (`scripts/directml_proof.py`: 24/24 tokens vs oracle and vs CPU, 24/24 stages, ≥ 15 % gain, 4 live runs, 60-minute loopback with load, one adapter LUID/driver throughout), then rebuild, re-run `test_cli_failures.py`/`gui_test.py`, `proof_report.py` and `package.py`. The CPU stays the default provider; the 1.7B model is GPU-only.
- Reports carry `engine.provider`, `engine.adapter_*`, `options.provider/adapter/large_model`; `proof_report.py` reads `reports/directml.json` and the `*-directml*` reports — never hard-code "CPU only" prose again.
- The executable resolves `models\`, `regression\`, DLLs relative to itself and never searches elsewhere or downloads anything; don't add fallbacks.
- Long loopback tests: an HDMI/DisplayPort audio endpoint disappears when the display sleeps (`AUDCLNT_E_DEVICE_INVALIDATED`); use a permanent/virtual endpoint or keep the display awake.
