# External clean Windows test — still required

Run the supplied archive on Windows 10 22H2 x64 and Windows 11 24H2 x64 as a standard user. Use a clean machine with Media Foundation, no Python, development tools or preinstalled Visual C++ redistributables. Disconnect networking before extraction. Do not install missing dependencies to make a failing package pass.

1. Record Windows build/edition, CPU, RAM, GPU and driver. Extract into a path containing spaces and Chinese characters, such as `C:\Users\Public\ASR 测试 folder`.
2. Run `AsrWin.exe --check-package --report package-check.json` and `AsrWin.exe --diagnostics --report diagnostics.json`. Every non-system dependency must come from this folder; graphics-driver modules may come from the Windows DriverStore.
3. Run `AsrWin.exe --verify regression\manifest.json --provider cpu --report verification.json`. Require 24 passing cases and exit code 0. Run a PCM WAV and a float WAV through `--benchmark` too.
4. Start the interface, play known Mandarin, English and mixed clips, then Stop during speech. Confirm it stays responsive, drains accepted audio, clears the provisional area and appends finals once. Repeat Start/Stop several times.
5. Save to a filename containing Chinese characters and spaces. Verify the text is UTF-8 with bytes `EF BB BF` at the beginning. Only finalized text should be saved.
6. While listening to the default playback device, switch the Windows default. Repeat by removing the selected device. Require a visible stop, drained audio and a usable restart. Test an explicitly selected device separately.
7. Listen for at least 60 minutes while browsing, editing documents and playing video. Save the explicit diagnostic report. Record first/subsequent caption lag, finalization delay, backlog, discontinuities, audio loss, CPU/GPU load and peak working set **and private bytes**. The initial memory target is 16,000,000,000 bytes; record failures honestly.
8. With the process stopped, temporarily rename one bundled model graph. Startup must clearly fail without locating a copy elsewhere or downloading anything. Restore it and rerun `--check-package`.
9. Verify the process performs no network requests/downloads using an external network monitor appropriate for the test machine. A connection snapshot alone is insufficient proof.

Return the reports, exit codes, observed failures and hardware/OS details. DirectML is experimental unless an explicit validated adapter/driver policy is supplied with the package. CPU operation must remain available. A passing run on the development machine does not satisfy this external gate.
