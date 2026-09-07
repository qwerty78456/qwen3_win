# Benchmark report (reference PC)

Measured on AMD Ryzen 7 9800X3D 8-Core Processor (8 cores / 16 threads), 31 GiB RAM, Microsoft Windows 11 IoT Enterprise LTSC build 26100. GPU: AMD Radeon(TM) Graphics, AMD Radeon RX 9060 XT (unused; CPU provider).

## File transcription (`--verify`, 24 fixtures, CPU)

- Speech fixtures: mean RTF 0.616, max RTF 1.186; mean encoder 0.176 s, mean prefill 0.480 s, mean decode 136.0 ms/token.
- Model load 22.5 s (includes SHA-256 of 10.0 GB of assets).

## Thread budget

| Threads | Mean RTF | Decode ms/token | Prefill s | Encoder s |
|---|---|---|---|---|
| 4 | 0.612 | 141.2 | 0.987 | 0.340 |
| 6 | 0.554 | 137.9 | 0.653 | 0.255 |
| 8 | 0.549 | 141.8 | 0.540 | 0.207 |
| 12 | 0.544 | 146.4 | 0.406 | 0.157 |
| 16 | 0.553 | 150.4 | 0.359 | 0.139 |

Selected: 6 threads. smallest thread count within 5% of the best mean RTF (12 threads: 0.544); leaves headroom for capture, preprocessing and interface threads

## Memory

- default: peak working set 15.25 GiB (16.37 GB), private 15.15 GiB (16.26 GB), load 32.6 s, inference 6.12 s.
- no-prepack: peak working set 14.76 GiB (15.85 GB), private 16.33 GiB (17.54 GB), load 3.4 s, inference 12.73 s.
- shared-prepack: peak working set 15.25 GiB (16.37 GB), private 15.15 GiB (16.26 GB), load 13.6 s, inference 6.11 s.

Selected: shared-prepack.

## Live pipeline

- live-english-0002: 3 finals, provisional lag p95 2.30 s (max 2.30 s), first caption p95 6.42 s, finalization p95 3.55 s, max backlog 5.60 s, 1 suspensions, stop reason "", peak working set 15.25 GiB (16.37 GB), CPU 5.82 cores average.
- live-mandarin-0002: 2 finals, provisional lag p95 3.00 s (max 3.00 s), first caption p95 3.90 s, finalization p95 5.69 s, max backlog 11.12 s, 1 suspensions, stop reason "", peak working set 15.25 GiB (16.37 GB), CPU 5.06 cores average.
- live-mandarin-0003: 1 finals, provisional lag p95 4.60 s (max 4.60 s), first caption p95 3.79 s, finalization p95 9.10 s, max backlog 14.74 s, 1 suspensions, stop reason "", peak working set 15.25 GiB (16.37 GB), CPU 5.55 cores average.
- live-mixed-0009: 1 finals, provisional lag p95 1.81 s (max 1.81 s), first caption p95 3.80 s, finalization p95 2.56 s, max backlog 4.60 s, 1 suspensions, stop reason "", peak working set 15.25 GiB (16.37 GB), CPU 6.10 cores average.
- capture-3min-load2: 20 finals, provisional lag p95 4.14 s (max 4.66 s), first caption p95 8.86 s, finalization p95 6.47 s, max backlog 15.00 s, 13 suspensions, stop reason "Duration elapsed", peak working set 15.25 GiB (16.37 GB), CPU 4.58 cores average.

## Acceptance target

Initial real-time target: p95 provisional lag ≤ 4 s, bounded backlog without overload stops, no application audio loss, peak process memory ≤ 16 GB on the reference PC. See the live pipeline section for the measured values and `docs/PROOF.md` §7 for failures and limitations.
