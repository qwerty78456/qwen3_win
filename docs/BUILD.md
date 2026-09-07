# Reproducible development build

Development uses Visual Studio Build Tools 2026 with the x64 C++ toolchain, Windows SDK, CMake, Rust/Cargo and Python 3.12. These are development prerequisites only; the end-user folder contains no Python or Rust environment.

Run commands from the source root. All dependency URLs resolve to immutable revisions or pinned package versions; downloaded files are checked against `dependencies.lock.json` on repeat builds.

```powershell
python scripts/bootstrap.py            # resumable; keeps partial model downloads
python scripts/native_deps.py
python scripts/reference_wheels.py
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

`bootstrap.py` is resumable. Do not remove completed files or partial model downloads to retry it. Build acquisition uses the network; the resulting executable has no download code.

## Reference environment and proof

The reference environment is separate from the application. Create `.venv` with Python 3.12, install the versions in `reference-environment.lock.txt`, and acquire the fixed fixtures with `scripts/acquire_fixtures.py` (refuses to change a frozen selection). Large wheels already downloaded into `.cache/wheels` should be installed by their explicit filenames so pip does not prefer the index copy.

```powershell
.venv\Scripts\python.exe scripts/reference.py --prepare-only      # mel_filters.bin, prompt_reference.json
.venv\Scripts\python.exe scripts/test_preprocessing.py            # 29 boundary/format/resampling cases
.venv\Scripts\python.exe scripts/test_fixture_frontend.py         # mel + speech gate on the 24 fixtures
.venv\Scripts\python.exe scripts/test_speech_gate.py
.venv\Scripts\python.exe scripts/test_cli_failures.py
.venv\Scripts\python.exe scripts/reference.py --threads 8         # official-model references and traces
python scripts/check_publisher_hashes.py
python scripts/model_manifest.py                                  # freeze model asset hashes
build\Release\AsrWin.exe --verify regression\manifest.json --model models\qwen3-asr-1.7b-fp32 --report reports\verification.json --trace traces\native
.venv\Scripts\python.exe scripts/compare_traces.py <fixture-id> --report reports\stages\<fixture-id>.json   # for every fixture
.venv\Scripts\python.exe scripts/encoder_diff.py --fixture mandarin-0000 --report reports\encoder-diff-mandarin-0000.json
.venv\Scripts\python.exe scripts/test_boundaries.py               # window edges, 15 s limit, repeated sessions
.venv\Scripts\python.exe scripts/memory_modes.py                  # decoder weight-sharing configurations
.venv\Scripts\python.exe scripts/benchmark_threads.py             # CPU thread budget
build\Release\AsrWin.exe --benchmark regression\english-0002.wav --simulate-live --model models\qwen3-asr-1.7b-fp32 --report reports\live-english-0002.json
.venv\Scripts\python.exe scripts/acquire_heldout.py               # held-out evaluation set (disjoint from regression/)
.venv\Scripts\python.exe scripts/reference.py --threads 8 --manifest evaluation/manifest.json
python scripts/acquire_0_6b.py                                    # 0.6B ONNX export (shipped); acquire_reference_0_6b.py fetches its official checkpoint
.venv\Scripts\python.exe scripts/evaluate_variants.py --variants fp32,int4,0.6b   # model-configuration decision (reports/variant-evaluation.json)
.venv\Scripts\python.exe scripts/make_playlist.py --repeats 21 --gap 2.0 --name playlist-60   # playback material for loopback tests
.venv\Scripts\python.exe scripts/gui_test.py --seconds 60         # interface driven through Win32 messages while ffplay renders fixtures
.venv\Scripts\python.exe scripts/live_capture_test.py --playlist playlist-1x --name 3min-load2 --load 2
.venv\Scripts\python.exe scripts/live_capture_test.py --playlist playlist-60 --name 60min-load2 --load 2
.venv\Scripts\python.exe scripts/proof_report.py                  # docs/PROOF.md, docs/BENCHMARK.md, docs/COMPATIBILITY.md
python scripts/package.py --version 0.1.0-tc1                     # dist/ folder, archive, reports/package.json
build\Release\AsrWin.exe --check-package                          # run inside the extracted package folder
```

`compare_traces.py` returns nonzero for missing stages, mismatched shapes, nonfinite values, incorrect prompt tokens or numerical divergence. Transcript verification alone does not complete the proof gate. Numerical tolerances, their change history and scoring normalization are in `verification-policy.json`.

The reference oracle applies the official encoder window mask that the pinned eager code path omits; `scripts/reference.py --global-attention` reproduces the unmasked path. See `docs/PROOF.md` §2.

## Executable modes

The application resolves model assets relative to its executable unless the development-only `--model` path override is supplied. Missing model graphs, weights or manifest entries fail explicitly. `--provider directml` is rejected unless `--experimental-directml` is present (benchmark experiments only); the interface always uses the CPU provider. `--skip-asset-check` and `--memory-mode` exist for development benchmarks.

Starting `AsrWin.exe` without arguments opens the capture interface (a console window flashes briefly when launched from Explorer because the executable keeps the console subsystem for its command-line modes).
