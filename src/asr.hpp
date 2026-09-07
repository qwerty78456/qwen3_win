#pragma once
#include "common.hpp"
#include "mel.hpp"
#include <atomic>
#include <functional>
#include <onnxruntime_cxx_api.h>
#include <tokenizers_cpp.h>
namespace asrwin {
struct EngineOptions {
  int threads=4;  // chosen by scripts/benchmark_threads.py for the shipped configuration
  int max_tokens=512;
  bool directml=false;
  int adapter=0;
  bool verify_assets=true;
  std::string memory_mode="shared-prepack";  // default | no-prepack | shared-prepack (decoder sessions share pre-packed weights)
  std::string variant="fp32";                // fp32 | int4 (decoder graphs; the encoder is FP32 in both)
  double slow_inference=0;                   // development fault injection: extra seconds per generation
  std::function<void(const std::string&)> progress;  // optional load-progress callback
};
struct Transcript {
  std::string text, raw, completion;  // completion: eos | token_limit | cancelled
  std::vector<int32_t> tokens;
  Json detail;
};
class Engine {
 public:
  Engine(const fs::path& model, EngineOptions options);
  // One generation owns fresh cache state. `cancel` is polled between decoder steps.
  Transcript transcribe(std::span<const float> audio, const fs::path& trace={},
                        const std::atomic<bool>* cancel=nullptr, int max_tokens=0);
  Json inspect() const;
  const EngineOptions& options() const { return options_; }
 private:
  fs::path model_;
  EngineOptions options_;
  Json config_;
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING,"AsrWin"};
  Ort::MemoryInfo cpu_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault)};
  std::unique_ptr<Ort::PrepackedWeightsContainer> prepacked_;
  std::unique_ptr<Ort::Session> encoder_, init_, step_;
  std::unique_ptr<tokenizers::Tokenizer> tokenizer_;
  std::unique_ptr<MappedFile> embeddings_;
  std::unique_ptr<MelExtractor> mel_;
  int hidden_=0, vocab_=0;
  bool half_=true, prefill_ids_=false;
  std::vector<int32_t> prefix_, suffix_, eos_;
  std::vector<float> embedding(int token) const;
  void report(const std::string& message) const { if (options_.progress) options_.progress(message); }
};
}
