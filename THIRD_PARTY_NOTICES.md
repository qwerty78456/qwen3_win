# Third-party notices

Runtime components are bundled; users do not install these dependencies.

| Component | Revision/version | Notice location |
|---|---|---|
| ONNX Runtime, CPU and DirectML providers | Microsoft.ML.OnnxRuntime.DirectML 1.24.4 | licenses/onnxruntime |
| DirectML | Microsoft.AI.DirectML 1.15.4 | licenses/directml |
| Qwen3-ASR-1.7B | Immutable revisions in dependencies.lock.json | licenses/qwen-reference |
| Qwen ONNX export implementation | Immutable revision in dependencies.lock.json | licenses/onnx-export |
| tokenizers-cpp, Hugging Face binding only | Immutable revision and tokenizer.Cargo.lock | licenses/tokenizers-cpp, licenses/rust |
| KissFFT | Immutable revision in dependencies.lock.json | licenses/kissfft |
| libfvad / WebRTC VAD | Immutable revision in dependencies.lock.json | licenses/libfvad |
| nlohmann JSON single header | Immutable revision in dependencies.lock.json | licenses/json |

Build-only PyTorch, Python and processor tooling are excluded from the portable application. The native executable links the tokenizer, FFT and VAD statically. Source snapshots and acquired binary hashes are in the lock files.

The regression audio and transcripts retain separate dataset licenses. FLEURS excerpts are attributed to the FLEURS authors/Google under [CC BY 4.0](https://huggingface.co/datasets/google/fleurs). ASCEND excerpts are attributed to the ASCEND authors/CAiRE under [CC BY-SA 4.0](https://huggingface.co/datasets/CAiRE/ASCEND). Fixture IDs and source revisions are recorded in regression/manifest.json; the held-out evaluation set in evaluation/manifest.json uses the same sources, revisions and licenses. Audio conversion to PCM16 WAV is documented there. ASCEND-derived fixtures remain under CC BY-SA 4.0. Deterministically generated non-speech fixtures are CC0 1.0.

Microsoft Visual C++ redistributable files must be copied from the development toolchain's authorized Redist directory when packaging. Only the native x64 files required by the executable and bundled DLLs are included. Packaging must retain their applicable Microsoft redistribution terms.
