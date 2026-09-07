# Archived 1.7B FP32 proof — notes

These reports were produced with the Qwen3-ASR-1.7B FP32 configuration before the smaller-configuration rule selected 0.6B FP32 (see `../variant-evaluation.json` and `../../shipped-model.json`). `PROOF-1.7b.md` and `BENCHMARK-1.7b.md` are the generated documents at the time of archiving; the JSON files are the underlying measurements.

## 60-minute loopback attempt (`capture-60min-load2.json`)

- Configuration: 1.7B FP32, 6 threads, shared pre-packed weights, two background busy processes, `playlist-60` rendered by ffplay to the default endpoint ("2 - VG279QE5A (AMD High Definition Audio Device)", HDMI monitor audio).
- Outcome: capture stopped after 882 s with `Capture failed: Loopback packet query failed (device lost?)`. The endpoint belongs to the monitor; the display went to sleep after the inactivity timeout and the audio endpoint was invalidated. The pipeline handled it as designed: the stop was visible, the active utterance was finalized (`final #195 stop`) and the run ended cleanly with 0 dropped frames and 0 overflow events. Later test scripts keep the display awake with `SetThreadExecutionState` (per-process request, no system setting change), and the executable now includes the HRESULT in the failure message.
- Measurements up to the stop (14.7 min, 98 non-empty finals): p95 provisional lag 4.58 s, p95 first caption 9.0 s, p95 finalization 8.7 s, max backlog 15 s, 65 provisional suspensions ("Catching up"), peak working set 15.25 GiB. This confirms the simulated-live finding that the 1.7B FP32 decoder (about 140 ms per token) cannot hold the 4 s provisional target on long Mandarin utterances with background load.
- The player process was not terminated when the capture ended early, so it kept rendering the playlist for another 45 minutes; a later interface test that overlapped with it captured mixed audio and was rerun. `scripts/live_capture_test.py` now terminates the player when the capture ends.
