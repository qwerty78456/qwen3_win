# Benchmark report (reference PC)

Measured on AMD Ryzen 7 9800X3D 8-Core Processor (8 cores / 16 threads), 31 GiB RAM, Microsoft Windows 11 IoT Enterprise LTSC build 26100. GPU: AMD Radeon(TM) Graphics, AMD Radeon RX 9060 XT (unused; CPU provider).

## File transcription (`--verify`, 24 fixtures, CPU)

- Speech fixtures: mean RTF 0.250, max RTF 0.492; mean encoder 0.121 s, mean prefill 0.175 s, mean decode 52.5 ms/token.
- Model load 3.7 s (includes SHA-256 of 4.1 GB of assets).

## Thread budget

| Threads | Mean RTF | Decode ms/token | Prefill s | Encoder s |
|---|---|---|---|---|
| 4 | 0.218 | 54.8 | 0.219 | 0.145 |
| 6 | 0.217 | 56.9 | 0.184 | 0.114 |
| 8 | 0.226 | 58.7 | 0.202 | 0.122 |
| 12 | 0.235 | 63.1 | 0.173 | 0.102 |
| 16 | 0.245 | 66.3 | 0.174 | 0.099 |

Selected: 4 threads. smallest thread count within 5% of the best mean RTF (6 threads: 0.217); leaves headroom for capture, preprocessing and interface threads

## Memory

- default: peak working set 5.88 GiB (6.32 GB), private 6.26 GiB (6.72 GB), load 3.7 s, inference 2.50 s.
- no-prepack: peak working set 5.88 GiB (6.31 GB), private 6.85 GiB (7.35 GB), load 1.2 s, inference 3.17 s.
- shared-prepack: peak working set 5.88 GiB (6.32 GB), private 6.26 GiB (6.72 GB), load 3.7 s, inference 2.48 s.

Selected: shared-prepack.

## Live pipeline

- live-english-0002: 3 finals, provisional lag p95 1.29 s (max 1.29 s), first caption p95 2.73 s, finalization p95 1.61 s, max backlog 1.17 s, 0 suspensions, stop reason "", peak working set 5.80 GiB (6.23 GB), CPU 2.20 cores average.
- live-mandarin-0002: 2 finals, provisional lag p95 1.58 s (max 1.58 s), first caption p95 2.79 s, finalization p95 2.49 s, max backlog 2.48 s, 0 suspensions, stop reason "", peak working set 5.80 GiB (6.23 GB), CPU 2.35 cores average.
- live-mandarin-0003: 1 finals, provisional lag p95 2.86 s (max 2.86 s), first caption p95 2.74 s, finalization p95 4.05 s, max backlog 1.06 s, 0 suspensions, stop reason "", peak working set 6.03 GiB (6.48 GB), CPU 2.93 cores average.
- live-mixed-0009: 1 finals, provisional lag p95 0.73 s (max 0.73 s), first caption p95 2.73 s, finalization p95 1.04 s, max backlog 0.44 s, 0 suspensions, stop reason "", peak working set 5.80 GiB (6.23 GB), CPU 2.62 cores average.
- capture-3min-load2: 20 finals, provisional lag p95 1.96 s (max 2.90 s), first caption p95 3.13 s, finalization p95 2.77 s, max backlog 3.81 s, 0 suspensions, stop reason "Duration elapsed", peak working set 6.14 GiB (6.59 GB), CPU 1.66 cores average.
- capture-3min-steam-load2: 20 finals, provisional lag p95 1.77 s (max 2.30 s), first caption p95 3.13 s, finalization p95 2.87 s, max backlog 3.65 s, 0 suspensions, stop reason "Duration elapsed", peak working set 6.14 GiB (6.59 GB), CPU 1.65 cores average.
- capture-60min-load2: 400 finals, provisional lag p95 2.01 s (max 2.97 s), first caption p95 3.12 s, finalization p95 3.46 s, max backlog 3.86 s, 0 suspensions, stop reason "Duration elapsed", peak working set 6.31 GiB (6.77 GB), CPU 1.65 cores average.

## Package size

- Extracted 3.84 GiB (4.12 GB), archive n/a (None).

## Acceptance target

Initial real-time target: p95 provisional lag ≤ 4 s, bounded backlog without overload stops, no application audio loss, peak process memory ≤ 16 GB on the reference PC. See the live pipeline section for the measured values and `docs/PROOF.md` §7 for failures and limitations.
