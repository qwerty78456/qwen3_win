#include "segmenter.hpp"
namespace asrwin {
Segmenter::Segmenter(Final final):final_(std::move(final)) {
    vad_=fvad_new();
    if (!vad_) throw std::bad_alloc();
    if (fvad_set_mode(vad_,2) || fvad_set_sample_rate(vad_,16000)) {
        fvad_free(vad_); vad_=nullptr;
        throw std::runtime_error("Cannot configure speech detection");
    }
    active_.audio.reserve(240000);
}
Segmenter::~Segmenter() { if (vad_) fvad_free(vad_); }
void Segmenter::consume(std::span<const float> samples) {
    if (finished_) throw std::runtime_error("Segmentation already finished");
    accepted_+=samples.size();
    for (float sample:samples) {
        if (!std::isfinite(sample)) throw std::runtime_error("Non-finite segmentation sample");
        pending_[pending_count_++]=sample;
        if (pending_count_==320) { frame(pending_,320); pending_count_=0; }
    }
}
void Segmenter::frame(std::span<const float> samples,size_t valid) {
    std::array<int16_t,320> pcm{};
    double energy=0;
    for(size_t i=0;i<320;++i) {
        float value=std::clamp(samples[i],-1.f,1.f);
        pcm[i]=static_cast<int16_t>(std::clamp(std::lround(value*32768),-32768l,32767l));
        energy+=value*value;
    }
    int decision=fvad_process(vad_,pcm.data(),pcm.size());
    if(decision<0) throw std::runtime_error("Speech detector rejected a 20 ms frame");
    // Reject near-digital silence and VAD hangover on isolated clicks. The
    // -80 dBFS floor retains quiet speech; VAD and the 100 ms start run reject noise.
    bool voiced=decision==1 && energy/320 >= 0.0001*0.0001;
    vad_frames_+=decision==1;
    voiced_frames_+=voiced;
    peak_rms_=std::max(peak_rms_,std::sqrt(energy/320));
    position_+=valid;
    recent_voice_.push_back(voiced);
    if(recent_voice_.size()>25) recent_voice_.pop_front();
    if(active_.audio.empty()) {
        for(size_t i=0;i<valid;++i) preroll_.push_back(samples[i]);
        while(preroll_.size()>4800) preroll_.pop_front();
        voiced_run_=voiced?voiced_run_+1:0;
        longest_run_=std::max(longest_run_,static_cast<uint64_t>(voiced_run_));
        if(voiced_run_<5) return;
        active_.id=next_id_++;
        active_.start_sample=position_-preroll_.size();
        active_.audio.assign(preroll_.begin(),preroll_.end());
        preroll_.clear();
        trailing_=0;
    } else {
        active_.audio.insert(active_.audio.end(),samples.begin(),samples.begin()+valid);
        trailing_=voiced?0:trailing_+valid;
    }
    active_.end_sample=position_;
    if(trailing_>=11200) emit(active_.audio.size(),"silence");
    else if(active_.audio.size()>=240000) {
        size_t cut=240000;
        // Prefer the latest non-voiced frame within 500 ms of the hard limit.
        for(size_t distance=0;distance<recent_voice_.size();++distance) {
            if(!recent_voice_[recent_voice_.size()-1-distance]) { cut=active_.audio.size()-distance*320; break; }
        }
        emit(std::min(cut,size_t(240000)),"utterance_limit");
    }
}
void Segmenter::emit(size_t count,const char* reason) {
    if(!count || count>active_.audio.size()) throw std::runtime_error("Invalid utterance boundary");
    Utterance output;
    output.id=active_.id;
    output.start_sample=active_.start_sample;
    output.end_sample=output.start_sample+count;
    output.boundary=reason;
    output.audio.assign(active_.audio.begin(),active_.audio.begin()+count);
    active_.audio.erase(active_.audio.begin(),active_.audio.begin()+count);
    active_.start_sample=output.end_sample;
    active_.id=next_id_++;
    trailing_=0;
    voiced_run_=0;
    preroll_.clear();
    recent_voice_.clear();
    final_(std::move(output));
}
void Segmenter::finish() {
    if(finished_) return;
    if(pending_count_) {
        std::fill(pending_.begin()+pending_count_,pending_.end(),0.f);
        frame(pending_,pending_count_);
        pending_count_=0;
    }
    if(!active_.audio.empty()) emit(active_.audio.size(),"stop");
    preroll_.clear();
    finished_=true;
}
std::optional<Utterance> Segmenter::snapshot() const {
    if(active_.audio.empty()) return std::nullopt;
    return active_;
}
Json Segmenter::statistics() const {
    return {{"vad_positive_frames",vad_frames_},{"energy_gated_positive_frames",voiced_frames_},
            {"longest_start_run_frames",longest_run_},{"peak_frame_rms",peak_rms_}};
}
}
