# Reproducible packaging

Everything below runs on the development machine. The output folder contains no Python, Rust or developer files and performs no downloads.

## Inputs

| Input | Source | Pinned by |
|---|---|---|
| `AsrWin.exe` | `scripts/build.ps1` (CMake, Visual Studio 2026 Build Tools, x64) | source tree |
| `onnxruntime.dll`, `onnxruntime_providers_shared.dll` | Microsoft.ML.OnnxRuntime.DirectML 1.24.4 native x64 | `dependencies.lock.json` (`.deps/archives/onnxruntime.zip` SHA-256) |
| `DirectML.dll` | Microsoft.AI.DirectML 1.15.4 | `native-dependency.lock.json` |
| `msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`, `vccorlib140.dll` | VC 14.51.36231 x64 CRT redistributable directory of the build toolchain | `scripts/build.ps1` (fails if the pinned version is absent) |
| `models/<shipped>/*` (see `shipped-model.json`) | the pinned ONNX export named in `shipped-model.json` (`onnx_lock_key` in `remote-assets.lock.json`), plus `mel_filters.bin`, `prompt_reference.json` and `manifest.json` generated from the official processor | `remote-assets.lock.json`, `shipped-model.json`, the model folder's `manifest.json` |
| `models/<large>/*` (`shipped-model.json` → `directml.large_model`) | the optional GPU-only model (`scripts/model_manifest.py --model-key large`) | same, `directml.large_model.onnx_lock_key` |
| `regression/*` | 24 frozen fixtures, human transcripts and official-model references of the shipped model and of the large model (`reference-1.7b/`, `manifest-1.7b.json`, installed by `scripts/prepare_large_model_references.py`) | `regression/manifest.json`, `regression/manifest-1.7b.json` |
| `licenses/*`, `THIRD_PARTY_NOTICES.md` | `scripts/collect_licenses.py` | source tree |

## Steps

```powershell
python scripts/bootstrap.py            # pinned sources, ONNX Runtime package, 1.7B model assets (resumable)
python scripts/native_deps.py          # pinned DirectML package, nlohmann/json
python scripts/collect_licenses.py     # notices for every compiled or shipped component
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
.venv\Scripts\python.exe scripts/reference.py --prepare-only   # mel_filters.bin, prompt_reference.json into the shipped model folder
python scripts/model_manifest.py       # freeze the shipped model's asset hashes (fails on incomplete downloads)
python scripts/model_manifest.py --model-key large   # the optional GPU-only model
python scripts/prepare_large_model_references.py     # its regression/evaluation references and manifests
python scripts/package.py --version 0.1.0-tc3
```

`scripts/package.py` copies the runtime files from `build/Release`, the model files listed in the model manifest, the regression set, notices and documentation into `dist/AsrWin-<version>-win-x64/`, writes `package-manifest.json` with the size and SHA-256 of every file, produces `dist/AsrWin-<version>-win-x64.zip` (deflate) and records extracted and compressed sizes in `reports/package.json`.

## Validating an extracted copy

```text
AsrWin.exe --check-package
```

`--check-package` reads `package-manifest.json` next to the executable and verifies every listed file by size and SHA-256. It returns nonzero and names the first missing or corrupt file. The model manifest is additionally verified every time the model loads.

Default model loading also checks the package manifest (which must list both model manifests). Development builds require an explicit `--model` path; there is no search of the working directory or parent development folders. The chosen model directory, CPU thread budget, default provider, DirectML validation flag and the large model's directory and thread budget are compiled from `shipped-model.json`. The only file the application writes on its own is the interface's compute-choice settings file in `%LOCALAPPDATA%\AsrWin`, outside the package folder.

The packager refuses to replace an existing version unless `--replace` is supplied and verifies the resolved destination stays inside `dist`. It publishes measured sizes and the archive hash in `AsrWin-<version>-win-x64-release-info.json` beside the ZIP.

## What the package must not contain

- No Python, PyTorch, Rust or reference-model files. `reference-model/`, `.venv/`, `.deps/` and `.cache/` are never copied.
- No absolute development paths. `AsrWin.exe --diagnostics` lists the loaded modules; on a clean machine every non-system module must resolve inside the package folder.
- No network access. The executable contains no download code; the clean-machine test records network activity while running the command-line verification and the interface.
