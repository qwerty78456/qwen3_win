# AsrWin

Offline Mandarin/English playback captions for Windows x64. One C++20 executable (`AsrWin.exe`) contains the Win32 interface, WASAPI loopback capture, preprocessing, speech gating, Qwen3-ASR ONNX inference (the shipped configuration is named in `shipped-model.json`), the file benchmark, regression verification, a native self-test and diagnostics. No helper processes, no Python at runtime, no downloads.

**Captures computer playback audio. Does not record the microphone.** Captions stay in the language that was spoken, including Mandarin–English switches inside one sentence; nothing is translated and nothing leaves the machine.

**Development status: test candidate.** The transcription proof, boundary/session tests, CPU thread budget, memory configuration, simulated live runs and real loopback captures were completed on the reference PC (`docs/PROOF.md`, `docs/BENCHMARK.md`). Clean-Windows verification on external machines is **not** done; `docs/COMPATIBILITY.md` lists exactly what was tested. No clean-install or real-time support claim is made for other machines.

---

## Contents

1. [What you get](#1-what-you-get)
2. [Requirements](#2-requirements)
3. [Deployment A — portable package on a target PC](#3-deployment-a--portable-package-on-a-target-pc)
4. [Deployment B — build the package from this repository](#4-deployment-b--build-the-package-from-this-repository)
5. [Using the interface](#5-using-the-interface)
6. [Command-line reference](#6-command-line-reference)
7. [Behaviour you should expect](#7-behaviour-you-should-expect)
8. [Configuration](#8-configuration)
9. [Re-running the verification suite](#9-re-running-the-verification-suite)
10. [Troubleshooting](#10-troubleshooting)
11. [Gates, repository layout and documents](#11-gates-repository-layout-and-documents)
12. [Privacy, model and licences](#12-privacy-model-and-licences)

---

## 1. What you get

| Item | Where |
|---|---|
| Portable folder / ZIP for end users | produced by `scripts/package.py` into `dist/AsrWin-<version>-win-x64[.zip]` |
| Source, build scripts, regression set, reports | this repository |
| Model weights (~16 GB of ONNX exports) | **not** in the repository — acquired by `scripts/bootstrap.py` / `scripts/acquire_0_6b.py`, bundled into the package |

The repository is deliberately without `models/`, `traces/`, `dist/`, `build/` and `.venv/` (see `.gitignore`). A clone therefore builds a package; it does not contain one. Distribute the ZIP produced by packaging, not the repository, to end users.

Reference build of the current test candidate:

| | |
|---|---|
| Package | `AsrWin-0.1.0-tc2-win-x64` |
| Files | 255 (`--check-package` verifies the 254 listed in `package-manifest.json`) |
| Extracted | 4,124,742,129 bytes |
| Archive | 1,967,563,388 bytes |
| Archive SHA-256 | `0d97c972fecc2a2e7d2edc1d393dfe26fac6c8451e7ab1cba827954dbe6873d2` |

Exact figures for any build are written next to the archive as `dist/AsrWin-<version>-win-x64-release-info.json`.

## 2. Requirements

**To run the packaged application**

- Windows 10 22H2 x64 or Windows 11 x64 with Media Foundation. Windows N editions without the Media Feature Pack are **not** supported: the Windows Audio Resampler is a Media Foundation component.
- About 15 GB free disk space after extraction (the package carries the shipped 0.6B model and the optional GPU-only 1.7B model); 8 GB RAM minimum for the default configuration. The shipped Qwen3-ASR-0.6B FP32 configuration peaked at 6.31 GiB working set during a 60-minute capture on the reference PC.
- A 64-bit CPU; 8 threads or more recommended. The application uses 4 inference threads by default and averaged 1.6–1.7 cores in the live runs.
- **No GPU is required.** The CPU is the default provider. A DirectX 12 GPU with enough dedicated memory can be selected in the interface's compute list or with `--provider directml`: about 5.7 GB for the shipped model and about 14.1 GB for the optional 1.7B model (1.5 × the model's graph size, checked against the adapter before it is offered). GPU acceleration is only offered on builds whose `shipped-model.json` marks DirectML as validated; `docs/COMPATIBILITY.md` names the exact adapter and driver it was validated on, and everything else is untested. The 1.7B model additionally needs about 24 GB of RAM (its process private bytes exceed 16 GB).
- Nothing to install: no runtime installer, no Visual C++ redistributable, no Python. Every non-system DLL ships in the folder.

**To build from source (development machine only)**

Visual Studio Build Tools 2026 with the x64 C++ toolchain and Windows SDK, CMake, Rust/Cargo (for `tokenizers-cpp`), Python 3.12, and about 60 GB of free space for model downloads, traces and packages. Python and Rust are build-time only; they are never shipped and the executable contains no download code.

## 3. Deployment A — portable package on a target PC

1. **Copy the archive** `AsrWin-<version>-win-x64.zip` and its `-release-info.json` to the target machine.
2. **Check the archive** before extracting:
   ```powershell
   Get-FileHash .\AsrWin-0.1.0-tc2-win-x64.zip -Algorithm SHA256
   ```
   The value must equal `archive_sha256` in the release-info file.
3. **Extract the whole folder** (right-click → Extract All, or `Expand-Archive`). Keep the folder intact: `AsrWin.exe` resolves `models\`, `regression\`, `docs\` and the DLLs relative to itself and never searches elsewhere. Do not copy the executable out on its own.
4. **Verify the copy** from inside the extracted folder:
   ```powershell
   .\AsrWin.exe --check-package --report package-check.json
   ```
   It prints `Package OK: <n> verified files` and returns 0. Any missing or altered file is named and the exit code is nonzero.
5. **Record the environment** (useful when reporting a problem):
   ```powershell
   .\AsrWin.exe --diagnostics --report diagnostics.json
   ```
6. **Optional smoke test** with a bundled fixture, without opening the interface:
   ```powershell
   .\AsrWin.exe --benchmark regression\mixed-0009.wav --report benchmark.json
   ```
7. **Start it**: double-click `AsrWin.exe`, or run it with no arguments. Loading takes a few seconds because the package manifest and every model asset are hashed at startup.

No administrator rights are needed; a standard user account is enough. Paths containing spaces and Chinese characters are part of the test checklist. Uninstalling is deleting the folder and, if you changed the compute choice, `%LOCALAPPDATA%\AsrWin` — nothing else is written outside the folder except the files you explicitly save.

**Distributing to several machines.** Copy the extracted folder as a whole (or the ZIP) to each machine and run `--check-package` there. Running the folder from a network share works but adds the share's latency to model loading; a local copy is preferable. There is no installer, service, registry key or scheduled task.

## 4. Deployment B — build the package from this repository

Run everything from the source root on the development machine. All dependency URLs resolve to immutable revisions or pinned package versions and are checked against `dependencies.lock.json` on repeat builds.

```powershell
python scripts/bootstrap.py            # pinned sources, ONNX Runtime, model assets (resumable — do not delete partials)
python scripts/native_deps.py          # pinned DirectML package, nlohmann/json
python scripts/collect_licenses.py     # notices for every compiled or shipped component
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

`build.ps1` produces `build\Release\AsrWin.exe`. The shipped model directory, the CPU thread budget, the default provider, the DirectML validation flag and the optional large model are compiled in from `shipped-model.json`.

Then prepare and freeze the model assets and assemble the package:

```powershell
python scripts/acquire_0_6b.py                                   # shipped 0.6B ONNX export (bootstrap.py fetches the 1.7B one)
.venv\Scripts\python.exe scripts/reference.py --prepare-only     # mel_filters.bin, prompt_reference.json into the model folder
python scripts/model_manifest.py                                 # freeze asset hashes; fails on incomplete downloads
python scripts/model_manifest.py --model-key large               # the optional GPU-only 1.7B model
python scripts/prepare_large_model_references.py                 # regression/manifest-1.7b.json and its official-model references
python scripts/check_publisher_hashes.py                         # compare against the publisher's hashes
python scripts/package.py --version 0.1.0-tc3 --replace
cd dist\AsrWin-0.1.0-tc3-win-x64 ; .\AsrWin.exe --check-package
```

`package.py` copies the runtime files from `build\Release`, the model files listed in each model manifest (the shipped model and the optional large model), the regression set with the references of both models, notices and documentation into `dist\AsrWin-<version>-win-x64\`, writes `package-manifest.json` with the size and SHA-256 of every file, produces the deflate ZIP and records sizes in `reports/package.json`. `docs/PACKAGING.md` lists every input and its pinning source; `docs/BUILD.md` covers the reference (oracle) environment used for proofs.

## 5. Using the interface

Start `AsrWin.exe` with no arguments. The window has, top to bottom: a playback-device list with **Refresh** and the **Start** / **Stop** / **Save…** / **Report…** buttons, the **compute list** (CPU or a GPU), the fixed notice *"Captures computer playback audio. Does not record the microphone."*, a status line, a one-line provisional caption box and the scrollable transcript.

1. **Wait for loading.** The status line shows `Loading model…` while the package manifest and model assets are hashed and the ONNX sessions are created (about 4–7 s on the reference PC). Start is enabled afterwards and the status reads `Ready (<compute choice>)`.
2. **Choose the output to listen to.** Either `Default playback device` or a named endpoint (speakers, headphones, HDMI, a virtual device). **Refresh** re-enumerates if you plug something in. With `Default playback device`, the default is resolved at the moment you press Start; if the default later changes or the device disappears, listening stops cleanly and the audio already accepted is still transcribed.
3. **Optionally choose the compute device.** The compute list starts with `CPU — Qwen3-ASR-0.6B (4 threads)`. On builds where DirectML is validated it also lists every DirectX 12 adapter with enough dedicated memory, once for the shipped model (`GPU: <adapter> — Qwen3-ASR-0.6B`) and, on adapters with about 14 GB or more, once for the larger model (`GPU: <adapter> — Qwen3-ASR-1.7B (higher accuracy)`). Changing the selection while not listening unloads the current model and loads the chosen one (a few seconds on the CPU, 10–30 s for the 1.7B model on the GPU); Start stays disabled meanwhile. If the GPU cannot be used, the status line shows the error, a message box explains it, and you simply select CPU again — nothing is switched automatically. The choice is remembered in `%LOCALAPPDATA%\AsrWin\settings.json` and used the next time you start the application (the command line `--provider`/`--adapter`/`--large-model` overrides it for that start).
4. **Press Start.** The status becomes `Listening: <device>`. Play anything — a video, a call, a local file. Captions follow the audio your PC plays back; your microphone is never opened.
5. **Read the captions.** The provisional box shows the sentence being spoken as it is being recognised (measured p95 lag 1.6–1.8 s on the reference PC; a `…` suffix marks a provisional cut off by the token limit). When the sentence ends, the provisional box clears and the finished text is appended to the transcript below — exactly once, never duplicated, never out of order.
6. **`Catching up: finalized-work backlog N s`** in the status line means transcription is behind the audio; provisional captions pause while final captions continue, and they resume automatically when the backlog falls back below 2 s. If the backlog ever passes 20 s the session stops instead of growing without bound, and the reason is shown in the status line.
7. **Press Stop.** The sentence in progress is finished and appended, then the status shows `Stopped: <reason>`. Start again whenever you want; sessions are independent and nothing from the previous session can appear in the next one.
8. **Save…** writes the transcript as UTF-8 text with a byte-order mark (`EF BB BF`), so Notepad and Excel read Chinese correctly. Only finalized text is saved; the provisional line is never written.
9. **Report…** writes a diagnostic JSON file with timings, backlog, capture statistics, memory, the compute selection (provider, adapter, model, the last load error if any) and the settings-file state. It contains **no audio and no transcript text**, so it is safe to attach to a bug report.

Audio lives only in memory and is released as soon as it has been transcribed (at most 60 s of unfinished audio is retained). Nothing is written to disk unless you press Save… or Report….

## 6. Command-line reference

`AsrWin.exe` keeps the console subsystem so the same executable serves the command line; launched from Explorer it frees the console immediately (a brief flash). Exactly one action may be given per invocation.

```text
AsrWin.exe                                                    interface
AsrWin.exe --help                                             usage
AsrWin.exe --benchmark input.wav [--report results.json]      transcribe a file
AsrWin.exe --benchmark input.wav --simulate-live [--pace 1.0] real-time replay through the live pipeline
AsrWin.exe --capture --device default --duration 60           real loopback capture
AsrWin.exe --verify regression\manifest.json                  24-fixture regression against the official references
AsrWin.exe --self-test regression\mandarin-0003.wav           native queue/lifecycle tests with a synthetic decoder
AsrWin.exe --check-package                                    verify package-manifest.json
AsrWin.exe --diagnostics | --devices | --inspect              environment, endpoints, loaded model
AsrWin.exe --preprocess in.wav | --segment in.wav | --tokenize "text"
```

| Option | Effect |
|---|---|
| `--report path.json` | write the full JSON result of the action |
| `--transcript out.txt` | (`--capture` / `--simulate-live`) write the final transcript, UTF-8 with BOM |
| `--trace dir` | dump stage tensors (`pcm`, `mel`, `encoder`, `input_embeds`, `logits_*`, `keys_*`, `values_*`) for numerical comparison |
| `--device default\|<name>\|<id>` | capture endpoint; an endpoint id matches exactly, otherwise the text is matched case-insensitively as a substring of the device names |
| `--duration <s>` | capture length (0 < s ≤ 86400) |
| `--pace <x>` | replay speed for `--simulate-live` (1.0 = real time) |
| `--tail-silence <s>` | silence appended to a replayed file so the last sentence closes |
| `--no-provisional`, `--provisional-max-tokens <n>` | disable or bound provisional captions |
| `--threads <1..16>` | inference threads (default from `shipped-model.json`: 4) |
| `--max-tokens <1..1024>` | decoder token limit per utterance |
| `--repeat <n>`, `--interleave other.wav` | repeated/interleaved sessions; the report asserts token-identical output |
| `--profile dir` | ONNX Runtime profiling output |
| `--model dir` | **development only** — use an unpackaged model directory (skips package verification) |
| `--memory-mode`, `--variant`, `--skip-asset-check`, `--slow-inference <s>`, `--large-model-dir dir`, `--settings path`, `--no-settings` | development benchmarks, fault injection, an unpackaged large model, and redirecting or disabling the interface settings file (tests) |
| `--provider cpu\|directml` | inference provider; `cpu` is the default from `shipped-model.json` |
| `--adapter default\|<n>` | DirectML adapter: `default` (the eligible adapter with the most dedicated memory) or a DXGI index from `--adapters`; software renderers and adapters below the model's memory requirement are refused |
| `--adapters` | list every DXGI adapter with index, LUID, driver version, dedicated memory and which models it is eligible for |
| `--large-model` | use the optional 1.7B model (`shipped-model.json` `directml.large_model`); the interface only offers it on a DirectML adapter |
| `--skip-adapter-check`, `--experimental-directml` | experiments only: bypass the video-memory rule; on builds where DirectML is not validated, `--provider directml` is rejected without `--experimental-directml` and an explicit `--adapter <n>` (the interface then shows GPU items only when started with the flag) |

Exit codes: **0** success, **1** invalid input or failure (the message is on stderr and, when `--report` was given, in the JSON), **2** the action ran but did not pass (failed verification, non-EOS completion, unstable repeats, a live run that missed its own success criteria).

WAV input must be integer PCM (8/16/24/32-bit) or 32-bit float; any sample rate and channel count is accepted and normalized to 16 kHz mono.

Examples:

```powershell
.\AsrWin.exe --devices --report devices.json
.\AsrWin.exe --adapters --report adapters.json
.\AsrWin.exe --capture --device "Speakers (Realtek(R) Audio)" --duration 300 --transcript meeting.txt --report meeting.json
.\AsrWin.exe --capture --provider directml --adapter default --duration 300 --report meeting-gpu.json
.\AsrWin.exe --verify regression\manifest.json --report verification.json
.\AsrWin.exe --verify regression\manifest-1.7b.json --large-model --provider directml --report verification-1.7b-gpu.json
.\AsrWin.exe --provider directml --large-model      # open the interface preselected on the GPU with the 1.7B model
```

## 7. Behaviour you should expect

**Capture.** Event-driven WASAPI shared-mode loopback on the selected render endpoint, with a silent render stream so the engine keeps pumping when the application itself is quiet. The capture callback only copies raw device bytes, arrival timestamps and flags into a lock-free ring; conversion to 16 kHz mono float happens on a separate preprocessing thread. Device removal, default-device change and glitch/discontinuity flags are detected and counted, not ignored.

**Segmentation.** libfvad mode 2 over 20 ms frames with an energy floor, a 100 ms voiced run to open an utterance, 300 ms of pre-roll kept in front of it, 700 ms of trailing silence to close it, and a hard 15 s limit that prefers the last quiet frame within the final 500 ms. Non-speech produces no caption.

**Scheduling.** One inference worker. The first provisional is requested after 2 s of utterance audio and then at most every 2 s; a final always pre-empts a provisional in flight; results that belong to a finished or superseded utterance are discarded. The backlog is the age of the oldest unfinished utterance: above 4 s provisional captions are suspended, below 2 s they resume, above 20 s the session stops. At most 60 s of unfinished audio is retained.

**Measured on the reference PC** (Ryzen 7 9800X3D, 31 GiB, Windows 11 build 26100; CPU provider unless stated; the DirectML rows use the AMD Radeon RX 9060 XT):

| Run | Finals | Provisional lag p95 | First caption p95 | Finalization p95 | Max backlog | Suspensions | Peak working set (private bytes) |
|---|---|---|---|---|---|---|---|
| 3-minute loopback + 2 load processes, CPU | 20 | 1.66 s | 3.16 s | 2.90 s | 3.64 s | 0 | 6.14 GiB (6.27 GiB) |
| 60-minute loopback + 2 load processes, CPU | 400 | 1.64 s | 3.09 s | 3.53 s | 3.70 s | 0 | 6.31 GiB (6.53 GiB) |
| 3-minute loopback + 2 load processes, DirectML 0.6B | 20 | 1.24 s | 2.60 s | 1.79 s | 2.26 s | 0 | 1.48 GiB (8.08 GiB) |
| 60-minute loopback + 2 load processes, DirectML 0.6B | 400 | 1.23 s | 2.49 s | 2.20 s | 2.33 s | 0 | 1.50 GiB (8.14 GiB) |

File transcription of the 24 fixtures on the CPU: mean RTF 0.249, max 0.486, decode 53 ms/token, model load 6.9 s including SHA-256 of 4.1 GB of assets. On DirectML the same 24 fixtures produced identical token sequences 33 % faster end to end (30.6 s → 20.4 s; decode 54 → 39 ms/token; encoder 5× faster on utterances longer than 8 s but slower than the CPU on clips under 4 s; prefill slower because the KV cache is returned to the CPU every step), and used up to 7.8 GB of GPU memory. The acceptance target (p95 provisional lag ≤ 4 s, bounded backlog, no audio loss, ≤ 16 GB peak) was met in every measured run. Full tables, the 1.7B-on-GPU rows and the DirectML criteria: `docs/BENCHMARK.md`, `docs/PROOF.md` §4.

## 8. Configuration

The only user setting is the compute choice made in the interface, stored as `%LOCALAPPDATA%\AsrWin\settings.json` (`{"schema":1,"provider":"cpu|directml","adapter_luid":"…","adapter_name":"…","large_model":false}`). It is written after a model has loaded successfully with that choice, read only by the interface, ignored with a visible note when it is unreadable, and overridden by `--provider`/`--adapter`/`--large-model` on the command line. Deleting the file restores the CPU default. Everything else is fixed at build time and by the command line.

- **`shipped-model.json`** names the shipped configuration (model directory, variant, thread budget, report root, default provider, the DirectML validation flag and the optional GPU-only large model). CMake and `scripts/shipped.py` read it, so changing configuration means editing that file and then re-running `scripts/build.ps1`, `scripts/model_manifest.py`, the proof scripts and `scripts/package.py`. The current value is Qwen3-ASR-0.6B FP32 with FP16 embedding storage, CPU provider, 4 threads, with Qwen3-ASR-1.7B FP32 as the large model.
- **Model choice.** Development started with the 1.7B FP32 export, whose complete proof is archived under `reports/proof-1.7b/`. Because the 1.7B decoder missed the provisional-latency target on CPU and peaked at 15.25 GiB, the plan's smaller-configuration rule was applied on a held-out set (`evaluation/`, `reports/variant-evaluation.json`): 1.7B int4 was rejected (+2.0 pp Mandarin CER), 0.6B FP32 was accepted (no category worse than the baseline, no new silence failures) and is what ships.
- **Threads and memory mode** were selected by measurement (`reports/benchmark-threads.json`, `reports/memory.json`): 4 intra-op threads, shared prepacked weights between the two decoder sessions. `--threads` and `--memory-mode` override them for experiments only.
- **DirectML** is gated by `shipped-model.json` → `directml.validated`. It may only be set to true from a passing `reports/directml.json`, written by `scripts/directml_proof.py`, which requires for every shipped model on the chosen adapter: 24/24 regression fixtures with token sequences identical to the official oracle and to the CPU run, 24/24 numerical stages within the tolerances of `verification-policy.json` (any provider-specific override is logged there), at least 15 % end-to-end file-transcription gain over the CPU, four passing simulated live runs, a passing 60-minute loopback capture with two background load processes, and one adapter LUID and driver version across all of it. The executable then also confirms at every GPU load that the device ONNX Runtime bound has the LUID of the adapter selected. The state of the current build and the measured numbers are in `docs/PROOF.md` §4, `docs/BENCHMARK.md` and `docs/COMPATIBILITY.md`; the 1.7B model is offered on the GPU only because on the CPU it misses the provisional-latency target (`docs/RESUME-AUDIT.md`).

## 9. Re-running the verification suite

The regression set and the official-model references ship with the package, so the transcription proof can be repeated anywhere:

```powershell
.\AsrWin.exe --verify regression\manifest.json --report verification.json
```

It requires 24 frozen fixtures (6 Mandarin, 6 English, 6 mixed, 6 non-speech) with complete references belonging to the same model revision, and passes only when the transcript matches after normalization, the token sequence is identical, decoding ended on EOS, and non-speech produced no text. Expect `24: PASS` lines and exit code 0.

The remaining evidence needs the development environment (Python oracle) and is described in `docs/BUILD.md`: stage-by-stage numerical comparison (`scripts/compare_traces.py` against `verification-policy.json`), boundary and repeated-session cases (`scripts/test_boundaries.py`), validator failure cases (`scripts/test_validator_failures.py`), the interface test driven through Win32 messages (`scripts/gui_test.py`), loopback capture scoring (`scripts/live_capture_test.py`) and report generation (`scripts/proof_report.py`).

## 10. Troubleshooting

| Symptom | Cause and action |
|---|---|
| `Package OK` fails, or startup reports a missing/altered file | The folder was split, partially copied, or a file was quarantined. Re-extract the whole archive and re-run `--check-package`. The application never looks for a replacement elsewhere and never downloads anything. |
| Startup error about the audio resampler | Windows N edition without the Media Feature Pack — unsupported. Install the Media Feature Pack or use a normal edition. |
| `Cannot list playback devices` or an empty device list | The Windows Audio service is stopped, or the session has no audio endpoint (some remote-desktop configurations). Start the service or select a device explicitly. |
| Listening stops with `device invalidated` (`0x88890004`) | The endpoint disappeared: a monitor with HDMI/DisplayPort audio went to sleep, a headset was unplugged, or the default device changed. Pick a permanent endpoint, or keep the display awake for long sessions. Audio already accepted is still transcribed. |
| No captions although audio is playing | Check that the selected endpoint is the one the audio is actually rendered to (a browser can be routed to a different device in Volume Mixer). Silence, music without speech and noise produce no captions by design. |
| `Catching up …` appears often | The machine is slower than the reference PC or heavily loaded. Final captions still arrive; close background load, or accept the higher lag. Persistent backlog above 20 s stops the session on purpose. |
| Session stopped with a buffer/overload reason | The capture ring or the backlog hit its stop threshold instead of growing unbounded. Restart; report `Report…` output if it repeats. |
| Memory around 6 GB | Expected for the shipped FP32 configuration — the weights are resident. 8 GB RAM is the practical minimum; the 1.7B model needs about 24 GB. |
| A GPU item is missing from the compute list | The build is not DirectML-validated, or no adapter has enough dedicated memory (`--adapters` shows the requirement and the reason per adapter). Software renderers are never listed. |
| `Error: … — select CPU in the compute list to continue` | The GPU could not load the model (driver, memory or DirectML failure). Select CPU; the failed choice is not remembered. Attach the `Report…` JSON, which records the error and the adapter. |
| A caption line ends with `[incomplete: …]` | The utterance hit the decoder token limit; the text is shown but marked, never presented as a successful transcript. |
| Saved file shows `ä½ å¥½` in another program | That program ignored the UTF-8 BOM. The file itself is correct UTF-8 with `EF BB BF`. |
| Exit code 2 from `--verify` | A fixture failed. The per-fixture reason is in the report JSON. |
| The build cannot relink `AsrWin.exe` (LNK1104) | A previous run of the application or a test is still holding the executable. Stop it before rebuilding. |

For anything else, attach `--diagnostics` output, the `Report…` JSON and the Windows build/CPU/RAM/GPU details.

## 11. Gates, repository layout and documents

| Gate | State | Evidence |
|---|---|---|
| 1. Complete transcription proof | done on the reference PC | `docs/PROOF.md`, `reports/verification.json`, `reports/stages/`, `reports/boundaries.json` |
| 2. One executable, portable package | built; sizes measured | `scripts/package.py`, `reports/package.json`, `docs/PACKAGING.md` |
| 3. Capture, scheduling, interface | implemented; automated interface and loopback tests on the reference PC | `src/capture.cpp`, `src/pipeline.cpp`, `src/gui.cpp`, `reports/gui-test.json`, `reports/capture-*.json` |
| 4. Correctness and performance gates | CPU measured; DirectML judged by `scripts/directml_proof.py` (state in `docs/COMPATIBILITY.md`) | `reports/benchmark-threads.json`, `reports/memory.json`, `reports/live-*.json`, `reports/directml.json`, `docs/BENCHMARK.md` |
| 5. Clean Windows verification and release | **pending external machines** | `docs/COMPATIBILITY.md`, checklist in `docs/CLEAN-WINDOWS-TEST.md` |

The late-night resume audit (native self-test, validator failure cases, the first DirectML probe, capture-ring rework) and the reconciliation of the two agent sessions are described in `docs/RESUME-AUDIT.md`; the DirectML evidence itself is produced by `scripts/directml_proof.py` (`reports/directml.json`).

**Layout**

- `src/` — application sources (`main.cpp` modes, `asr.cpp` runner, `compute.cpp` DXGI adapter enumeration and the DirectML eligibility rule, `settings.cpp` the interface settings file, `mel.cpp` features, `audio.cpp` WAV/resampler, `segmenter.cpp` speech gate, `capture.cpp` WASAPI loopback, `pipeline.cpp` scheduler, `gui.cpp` interface, `scoring.cpp` CER/WER/MER, `self_test.cpp` native tests).
- `scripts/` — build-time acquisition, reference oracle, tests, benchmarks, packaging and report generation (`docs/BUILD.md`).
- `regression/` — 24 frozen fixtures with human transcripts and official-model references; `evaluation/` — the held-out set used for the configuration decision.
- `reports/` — measured JSON reports; `docs/` — generated proof, benchmark and compatibility documents.
- Lock files: `dependencies.lock.json` (sources, model assets, hashes), `remote-assets.lock.json` (publisher hashes), `native-dependency.lock.json` (DirectML), `tokenizer.Cargo.lock`, `reference-environment.lock.txt`, `verification-policy.json`.

**Documents**

| File | Contents |
|---|---|
| `docs/PROOF.md` | the transcription proof: fixtures, stage comparison, boundaries, gaps and limitations |
| `docs/BENCHMARK.md` | thread budget, memory configuration, live and capture measurements |
| `docs/COMPATIBILITY.md` | what was tested and on what, and what is untested |
| `docs/BUILD.md` | reproducible development build and the reference oracle environment |
| `docs/PACKAGING.md` | package inputs, pinning and validation |
| `docs/PACKAGE-README.md` | the short readme shipped inside the package |
| `docs/CLEAN-WINDOWS-TEST.md` | the external clean-machine checklist for gate 5 |
| `docs/RESUME-AUDIT.md` | the audit session, the DirectML probe and the configuration decision |

## 12. Privacy, model and licences

The application opens the playback loopback of the endpoint you choose and never the microphone. Audio stays in process memory and is released after transcription; the transcript is written only when you press Save…, the diagnostic report contains no audio or text, the only other file it writes is the compute-choice settings file in `%LOCALAPPDATA%\AsrWin`, and the executable contains no network or download code (`docs/CLEAN-WINDOWS-TEST.md` step 9 has the external check for this).

The shipped models are pinned Qwen3-ASR ONNX exports (repositories and revisions in `shipped-model.json` and `remote-assets.lock.json`), run with ONNX Runtime 1.24.4 (Microsoft.ML.OnnxRuntime.DirectML native x64; CPU provider by default, DirectML when selected). The encoder uses the model's designed 8-second attention windows; `docs/PROOF.md` §2 documents the difference from the pinned official eager code path and how the reference oracle accounts for it. Notices for every bundled component, and the attribution for the FLEURS and ASCEND excerpts used as fixtures, are in `THIRD_PARTY_NOTICES.md` and `licenses/`.
