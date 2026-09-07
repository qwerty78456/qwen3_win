#include "mel.hpp"
#include <numbers>
namespace asrwin {
std::vector<float> model_samples(std::span<const float> samples) {
 if(samples.empty())throw std::runtime_error("Cannot transcribe empty audio");
 std::vector<float> result(samples.begin(),samples.end());
 // Minimum 1.0 s (100 mel frames = one full encoder conv window). The pinned ONNX export pads a partial
 // conv chunk to 100 frames inside the graph, whereas the official code convolves a lone sub-second chunk
 // unpadded; feeding at least one full chunk keeps both paths identical for short inputs.
 result.resize(std::max<size_t>(16000,result.size()),0.f);
 return result;
}
MelExtractor::MelExtractor(const fs::path& p):filters_(read_floats(p)){
 if(filters_.size()!=128*201)throw std::runtime_error("Expected 128 x 201 mel filter coefficients");
 for(int i=0;i<400;++i)window_[i]=static_cast<float>(0.5-0.5*std::cos(2.0*std::numbers::pi*i/400));
 fft_=kiss_fftr_alloc(400,0,nullptr,nullptr);if(!fft_)throw std::bad_alloc();
}
MelExtractor::~MelExtractor(){free(fft_);}
int64_t MelExtractor::audio_tokens(int64_t frames){auto rem=frames%100;for(int i=0;i<3;++i)rem=(rem+1)/2;return frames/100*13+rem;}
Mel MelExtractor::compute(std::span<const float> samples) const {
 if(samples.size()<201)throw std::runtime_error("Audio is too short for reflect-padded STFT (need 201 samples)");
 Mel m;m.frames=static_cast<int64_t>(samples.size()/160);m.values.resize(128*m.frames);
 std::array<float,400> frame;std::array<kiss_fft_cpx,201> spectrum;std::array<float,201> power;
 for(int64_t t=0;t<m.frames;++t){for(int i=0;i<400;++i){int64_t pos=t*160+i-200;if(pos<0)pos=-pos;else if(pos>=static_cast<int64_t>(samples.size()))pos=2*static_cast<int64_t>(samples.size())-2-pos;frame[i]=samples[static_cast<size_t>(pos)]*window_[i];}kiss_fftr(fft_,frame.data(),spectrum.data());for(int k=0;k<201;++k)power[k]=spectrum[k].r*spectrum[k].r+spectrum[k].i*spectrum[k].i;
  for(int bin=0;bin<128;++bin){float sum=0;for(int k=0;k<201;++k)sum+=filters_[bin*201+k]*power[k];m.values[bin*m.frames+t]=std::log10(std::max(sum,1e-10f));}
 }
 float floor=*std::max_element(m.values.begin(),m.values.end())-8.f;for(auto& v:m.values)v=(std::max(v,floor)+4.f)/4.f;return m;
}
}
