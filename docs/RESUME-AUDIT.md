# Resume audit (night of 2026-09-07 to 2026-09-08)

Two agent sessions worked on this tree in sequence after the first run ran out of credit: the Claude session that completed gates 1–4 on the reference PC, and a separate Codex session started by a colleague at about 23:37 that audited and extended the result while the Claude session was paused. This note records what the audit changed, what evidence it produced, and how the tree was reconciled afterwards.

## What the audit added (kept)

- `src/self_test.cpp` and `AsrWin.exe --self-test speech.wav`: native queue, lifecycle and Unicode tests with a synthetic decoder (ring wrap and reserved capacity, deferred PCM conversion of raw packets, silent packets without data, stop/drain/restart without stale or duplicate finals, forced 15 s splits). Result: 5/5 (`reports/resume-audit/native-self-test.json`).
- Capture ring rework: the capture thread now stores raw device bytes, per-frame arrival timestamps (from the packet QPC), flags and format; conversion to float happens on the preprocessing thread. Device position gaps and resets are counted (`device_gap_frames`, `position_resets`).
- Package verification on default model load (`verify_package`), single-action argument validation, `--profile` for ONNX Runtime profiling, `--self-test`.
- `scripts/test_validator_failures.py`: synthetic corruption cases proving `compare_traces.py` fails on missing stages, shape mismatches, non-finite values and divergence.
- `docs/CLEAN-WINDOWS-TEST.md`: the external clean-machine checklist for gate 5.
- `scripts/diagnose_noise.py`, `reports/resume-audit/noise-*.json`: isolation of the `low-noise` K/V stage deviation (0.6B) using the official model; the tolerance was not changed.

## DirectML probe (the first evidence; superseded by `scripts/directml_proof.py`)

The audit's `scripts/test_directml.py` (since replaced by `scripts/directml_proof.py`) ran the 1.7B FP32 graphs with `--provider directml --experimental-directml --adapter 0` on the AMD Radeon RX 9060 XT (16 GB, driver 32.0.31041.1004). One benchmark fixture (`english-0002`) produced the correct transcript at RTF 0.54 with peak working set 7.3 GiB and private bytes 17 GiB (`reports/resume-audit/directml-1.7b.json`, profiles under `directml-profile/`). The audit also ran the full 24-fixture regression of the 1.7B graphs on the same adapter (`reports/resume-audit/verification-1.7b-directml.json`): 24/24 passed with token sequences identical to the CPU run and to the oracle, total inference 77.8 s on the CPU against 51.6 s on DirectML (1.51×, decode 140.7 → 89.2 ms/token), private bytes up to 17.6 GiB. A 60-minute DirectML capture was then started about four minutes **after** that regression (log timestamps 23:49:47 and 23:53:48) without background load and was stopped after about 9.5 minutes (`reports/resume-audit/capture-directml-60min.log`, last event at 567 s; no JSON report was written) because the operator observed the GPU memory fully occupied. No stage-trace comparison existed for DirectML at that point. DirectML therefore stayed disabled in the interface and rejected on the command line without `--experimental-directml` and an explicit adapter until the later evidence run (`reports/directml.json`, `docs/PROOF.md` §4), which added the missing stage comparison, the sustained capture with load, GPU-memory measurement and the adapter-identity check.

## Configuration decision

The audit set `shipped-model.json` back to Qwen3-ASR-1.7B FP32 on the colleague's instruction and started collecting a 1.7B evidence set under `reports/current-1.7b/`. The owner then asked for the plan to be finished as written. The plan's rule selects the configuration on the held-out set (`reports/variant-evaluation.json`): 1.7B FP32 misses the p95 4 s provisional-lag target on CPU and peaks at 15.25 GiB, 1.7B int4 is rejected (+2.0 pp Mandarin CER), 0.6B FP32 is accepted and meets the target (60-minute loopback: p95 provisional 2.0 s, finalization 3.5 s, no suspensions, 6.3 GiB). `shipped-model.json` was therefore restored to 0.6B FP32; the 1.7B proof remains archived under `reports/proof-1.7b/` and `reports/current-1.7b/`. Switching configurations is an edit of `shipped-model.json` followed by `model_manifest.py`, the proof scripts and `package.py`.

## Reconciliation

After the takeover the executable was rebuilt from the audited sources with the 0.6B defaults, and every model-dependent and live check was rerun with that binary (verification and stages, boundaries, self-test, validator failure tests, invalid-input cases, simulated live runs, interface test, loopback captures including the 60-minute run). The package was rebuilt as `0.1.0-tc2` and verified with `--check-package`. Results are in `docs/PROOF.md` and `docs/BENCHMARK.md`.
