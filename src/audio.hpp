#pragma once
#include "common.hpp"
#include <mftransform.h>
namespace asrwin {
struct Audio { std::vector<float> samples; uint32_t sample_rate=16000; Json source; };
Audio read_wav(const fs::path& path);
class Resampler {
 public:
  explicit Resampler(uint32_t input_rate);
  ~Resampler();
  std::vector<float> process(std::span<const float> input, bool finish=false);
 private:
  void drain(std::vector<float>& result);
  ComPtr<IMFTransform> transform_;
  uint32_t rate_; uint64_t input_frames_=0; bool finished_=false;
};
Audio normalize_audio(Audio audio);
}
