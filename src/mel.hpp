#pragma once
#include "common.hpp"
#include <kiss_fftr.h>
namespace asrwin {
std::vector<float> model_samples(std::span<const float> samples);
struct Mel { std::vector<float> values; int64_t frames=0; };
class MelExtractor {
 public:
  explicit MelExtractor(const fs::path& filters);
  ~MelExtractor();
  Mel compute(std::span<const float> samples) const;
  static int64_t audio_tokens(int64_t frames);
 private:
  std::vector<float> filters_;std::array<float,400> window_;kiss_fftr_cfg fft_=nullptr;
};
}
