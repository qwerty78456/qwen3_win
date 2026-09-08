# Compatibility matrix

Only configurations listed as tested were actually exercised. Everything else is untested; no support claim is made for it.

| Windows build / edition | Architecture | CPU | RAM | GPU / driver | Provider | Tested how | Result |
|---|---|---|---|---|---|---|---|
| Microsoft Windows 11 IoT Enterprise LTSC 10.0.26100 (build 26100) — development host, not a clean install | x64 | AMD Ryzen 7 9800X3D 8-Core Processor | 31 GiB | AMD Radeon(TM) Graphics 32.0.21045.5002; AMD Radeon RX 9060 XT 32.0.31041.1004 (present, unused) | CPU | transcription proof (`--verify`, 24 fixtures), boundary/session cases, 4 simulated live runs, 3 WASAPI loopback captures, interface test | proof incomplete; live measurements in docs/BENCHMARK.md |
| Windows 10 22H2 x64 | x64 | any | ≥16 GB | any | CPU | **not tested** | untested |
| Windows 11 24H2 x64 (clean install, standard user, offline) | x64 | any | ≥16 GB | any | CPU | **not tested** — pending external clean-machine run | untested |
| Windows N editions without Media Feature Pack | x64 | any | any | any | CPU | excluded (Windows Audio Resampler requires Media Foundation) | unsupported |
| Any | x64 | any | any | any | DirectML | not enabled in this version | unsupported |

## What "tested" covered on the development host

- Command-line transcription of the 24 frozen fixtures against the official model, stage-by-stage numerical comparison, boundary and repeated-session cases.
- Live pipeline: simulated real-time replay through the capture ring and scheduler, real WASAPI loopback capture of a player rendering to the default endpoint with background CPU load, and the interface driven through Win32 messages.
- The package manifest check (`--check-package`) on the assembled folder.

## Not yet covered

- Clean Windows 10 22H2 / Windows 11 24H2 machines without developer tools or Visual C++ redistributables (release gate), including paths with spaces and Chinese characters on such machines.
- Device removal / default-device change during a long live session (the handling exists but was not exercised by an automated test).
- Any GPU acceleration.
