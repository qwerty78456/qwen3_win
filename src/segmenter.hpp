#pragma once
#include "common.hpp"
#include <deque>
#include <functional>
#include <fvad.h>
namespace asrwin {
struct Utterance {
    uint64_t id=0, start_sample=0, end_sample=0;
    std::vector<float> audio;
    std::string boundary;
};
class Segmenter {
public:
    using Final=std::function<void(Utterance&&)>;
    explicit Segmenter(Final final);
    ~Segmenter();
    Segmenter(const Segmenter&)=delete;
    Segmenter& operator=(const Segmenter&)=delete;
    void consume(std::span<const float> samples);
    void finish();
    std::optional<Utterance> snapshot() const;
    uint64_t accepted_samples() const { return accepted_; }
    Json statistics() const;
private:
    void frame(std::span<const float> samples, size_t valid);
    void emit(size_t count, const char* reason);
    Fvad* vad_=nullptr;
    Final final_;
    std::array<float,320> pending_{};
    size_t pending_count_=0, trailing_=0, voiced_run_=0;
    uint64_t accepted_=0, position_=0, next_id_=1;
    uint64_t vad_frames_=0, voiced_frames_=0, longest_run_=0;
    double peak_rms_=0;
    std::deque<float> preroll_;
    std::deque<bool> recent_voice_;
    Utterance active_;
    bool finished_=false;
};
}
