#include "pipeline.hpp"
namespace asrwin {
FileSource::FileSource(Audio audio, double pace, double tail_silence)
  :audio_(std::move(audio)),pace_(pace),tail_(tail_silence),ring_(1,audio_.sample_rate*2,audio_.sample_rate/10) {}
FileSource::~FileSource() { stop(); if (thread_.joinable()) thread_.join(); }
void FileSource::start() {
  if (started_.exchange(true)) return;
  thread_=std::thread([this]{
    const uint32_t rate=audio_.sample_rate, packet=rate/100;  // 10 ms packets like a device period
    std::vector<float> zeros(packet,0.f);
    size_t total=audio_.samples.size(), tail=size_t(tail_*rate);
    auto t0=Clock::now();
    size_t count=0;
    for (size_t offset=0; offset<total+tail && !stop_.load(); offset+=count) {
      count=std::min<size_t>(packet,total+tail-offset);
      const float* data=offset<total?audio_.samples.data()+offset:zeros.data();
      if (offset<total && offset+count>total) count=total-offset;
      if (pace_>0) {
        auto due=t0+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(offset/double(rate)/pace_));
        auto now=Clock::now();
        if (now<due) std::this_thread::sleep_until(due);
        else { double late=std::chrono::duration<double>(now-due).count(); if (late>0.05) { ++late_packets_; max_late_=std::max(max_late_.load(),late); } }
      }
      bool stored=false;
      auto packet_end=pace_>0?t0+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((offset+count)/double(rate)/pace_)):Clock::now();
      while (!(stored=ring_.push(data,static_cast<uint32_t>(count),packet_end,rate))) {
        if (pace_>0) { ++overflow_; stop_=true; break; }  // a real device would have overrun: surface it
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (stop_.load()) break;
      }
      if (!stored) break;
      ++packets_; frames_+=count;
      if (pace_>0 && ring_.stop_threshold_reached()) { ++overflow_; stop_=true; }
    }
    finished_=true; ring_.notify();
  });
}
void FileSource::stop() { stop_=true; if (!started_) finished_=true; ring_.notify(); }
Json FileSource::statistics() const {
  return {{"source","wav-replay"},{"sample_rate",audio_.sample_rate},{"channels",1},{"pace",pace_},{"tail_silence_seconds",tail_},
          {"packets",packets_.load()},{"frames",frames_.load()},{"late_packets",late_packets_.load()},{"max_late_seconds",max_late_.load()},
          {"overflow_events",overflow_.load()},{"ring_peak_used_frames",ring_.peak_used_frames()},{"ring_capacity_frames",ring_.capacity_frames()}};
}

LivePipeline::LivePipeline(Engine& engine, Sink sink, PipelineOptions options)
  :LivePipeline([&engine](std::span<const float> samples,const std::atomic<bool>* cancel,int limit){ return engine.transcribe(samples,{},cancel,limit); },std::move(sink),options) {}
LivePipeline::LivePipeline(Decode decode, Sink sink, PipelineOptions options):decode_(std::move(decode)),sink_(std::move(sink)),options_(options) {}
LivePipeline::~LivePipeline() { if (running_.load()) stop("Pipeline destroyed"); wait(); source_.reset(); }
void LivePipeline::emit(CaptionEvent e) { e.session=session_; std::lock_guard<std::mutex> lock(sink_mutex_); if (sink_) sink_(e); }
uint64_t LivePipeline::start(std::unique_ptr<AudioSource> source, uint64_t session) {
  if (running_) throw std::runtime_error("Pipeline already running");
  wait();
  if (!source) throw std::runtime_error("Missing audio source");
  running_=true;
  session_=session; source_=std::move(source); started_at_=Clock::now();
  { FILETIME c{},x{},k{},u{}; GetProcessTimes(GetCurrentProcess(),&c,&x,&k,&u);
    cpu_at_start_=((uint64_t(k.dwHighDateTime)<<32|k.dwLowDateTime)+(uint64_t(u.dwHighDateTime)<<32|u.dwLowDateTime))/1e7; }
  finals_.clear(); provisional_.reset(); pending_ends_.clear(); in_flight_end_.reset(); queued_seconds_=in_flight_seconds_=max_backlog_=max_queued_seconds_=retained_peak_=0; suspended_=drained_=false;
  last_final_queued_=revision_=position16k_=finals_done_=provisionals_done_=provisionals_cancelled_=provisionals_skipped_=suspensions_=0;
  provisional_progress_.clear(); stopping_=false; stop_reason_.clear(); error_.clear();
  cancel_provisional_=false; capture_statistics_=Json(); segmenter_statistics_=Json();
  { std::lock_guard<std::mutex> lock(timeline_mutex_); timeline_.clear(); }
  segmenter_=std::make_unique<Segmenter>([this](Utterance&& u){
    std::unique_lock<std::mutex> lock(mutex_);
    double dur=u.audio.size()/16000.0;
    last_final_queued_=std::max(last_final_queued_,u.id);
    provisional_progress_.erase(u.id);
    if (provisional_ && provisional_->utterance<=last_final_queued_) { provisional_.reset(); ++provisionals_skipped_; }
    pending_ends_.push_back(u.end_sample);
    finals_.push_back(Final{std::move(u)}); queued_seconds_+=dur;
    cancel_provisional_.store(true);  // final work takes priority over an in-flight provisional decode
    review_backlog_locked(lock);
    cv_.notify_all();
  });
  try {
    source_->start();
    preprocess_=std::thread([this]{ preprocess(); });
    worker_=std::thread([this]{ work(); });
  } catch (...) {
    stopping_=true; source_->stop(); wait(); running_=false;
    throw;
  }
  emit({CaptionEvent::Kind::Status,0,0,0,"Listening"});
  return session_;
}
void LivePipeline::stop(const std::string& reason) {
  if (!running_.load() || stopping_.exchange(true)) return;
  { std::lock_guard<std::mutex> lock(mutex_); stop_reason_=reason; }
  cancel_provisional_=true;
  if (source_) source_->stop();
  cv_.notify_all();
}
void LivePipeline::wait() {
  auto self=std::this_thread::get_id();
  if (preprocess_.joinable() && preprocess_.get_id()!=self) preprocess_.join();
  if (worker_.joinable() && worker_.get_id()!=self) worker_.join();
}
void LivePipeline::record_timeline(uint64_t sample16k, Clock::time_point time) {
  std::lock_guard<std::mutex> lock(timeline_mutex_);
  timeline_.emplace_back(sample16k,time);
  uint64_t horizon=uint64_t((options_.retained_limit+15)*16000);
  while (timeline_.size()>2 && sample16k-timeline_.front().first>horizon) timeline_.pop_front();
}
Clock::time_point LivePipeline::capture_time(uint64_t sample16k) const {
  std::lock_guard<std::mutex> lock(timeline_mutex_);
  if (timeline_.empty()) return Clock::now();
  auto it=std::lower_bound(timeline_.begin(),timeline_.end(),sample16k,[](const auto& entry,uint64_t value){ return entry.first<value; });
  return it==timeline_.end()?timeline_.back().second:it->second;
}
double LivePipeline::backlog_locked() const {
  std::optional<uint64_t> oldest=in_flight_end_;
  if (!oldest && !pending_ends_.empty()) oldest=pending_ends_.front();
  if (!oldest) return 0.0;
  return std::max(0.0,std::chrono::duration<double>(Clock::now()-capture_time(*oldest)).count());
}
void LivePipeline::review_backlog_locked(std::unique_lock<std::mutex>& lock) {
  double backlog=backlog_locked();
  max_queued_seconds_=std::max(max_queued_seconds_,queued_seconds_);
  max_backlog_=std::max(max_backlog_,backlog);
  bool notify_suspend=false, notify_resume=false, overload=false;
  if (!suspended_ && backlog>options_.suspend_backlog) { suspended_=true; ++suspensions_; notify_suspend=true; }
  else if (suspended_ && backlog<options_.resume_backlog) { suspended_=false; notify_resume=true; }
  if (backlog>options_.overload_backlog && !stopping_.load()) overload=true;
  lock.unlock();
  if (notify_suspend) emit({CaptionEvent::Kind::Status,0,0,0,"Catching up: finalized-work backlog "+std::to_string(int(backlog+0.5))+" s"});
  if (notify_resume && !stopping_.load()) emit({CaptionEvent::Kind::Status,0,0,0,"Listening"});
  if (overload) stop("Overload: finalized-work backlog exceeded "+std::to_string(int(options_.overload_backlog))+" s");
  lock.lock();
}
void LivePipeline::preprocess() {
  ComScope com;
  try {
    RawRing& ring=source_->ring();
    const uint32_t rate=source_->sample_rate(), channels=source_->channels();
    Resampler resampler(rate);
    std::vector<float> raw, mono;
    const size_t chunk=std::max<size_t>(rate/50,160);  // 20 ms per pass
    raw.reserve(chunk*channels); mono.reserve(chunk);
    uint64_t raw_frames=0;
    auto raw_end=started_at_;
    auto handle=[&](std::span<const float> samples16k){
      if (samples16k.empty()) return;
      position16k_+=samples16k.size();
      auto normalized_end=raw_end-std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(raw_frames/double(rate)-position16k_.load()/16000.0));
      record_timeline(position16k_,normalized_end);
      segmenter_->consume(samples16k);
      double active=segmenter_->active_samples()/16000.0;
      std::unique_lock<std::mutex> lock(mutex_);
      segmenter_statistics_=segmenter_->statistics();
      double retained=active+queued_seconds_+in_flight_seconds_+(provisional_?provisional_->audio.size()/16000.0:0.0);
      retained_peak_=std::max(retained_peak_,retained);
      review_backlog_locked(lock);  // the backlog is an age: re-evaluate it as audio time advances
      if (retained>options_.retained_limit && !stopping_.load()) { lock.unlock(); stop("Retained audio exceeded "+std::to_string(int(options_.retained_limit))+" s"); return; }
      if (options_.provisional && active>0 && !suspended_ && !stopping_.load()) {
        uint64_t id=segmenter_->active_id();
        uint64_t have=segmenter_->active_samples(), last=provisional_progress_.count(id)?provisional_progress_[id]:0;
        uint64_t first=uint64_t(options_.provisional_after*16000), step=uint64_t(options_.provisional_interval*16000);
        if (have>=first && (last==0 || have-last>=step)) {
          auto snapshot=segmenter_->snapshot();
          provisional_progress_[snapshot->id]=have;
          provisional_=Provisional{snapshot->id,++revision_,snapshot->start_sample,std::move(snapshot->audio)};
          cv_.notify_all();
        }
      }
    };
    bool announced_stop=false;
    for (;;) {
      // Reserve room for a 15 s final/snapshot copy and the resampler tail. Stop
      // the producer first, then let final work free memory before draining more.
      {
        std::unique_lock<std::mutex> lock(mutex_);
        double retained=segmenter_->active_samples()/16000.0+queued_seconds_+in_flight_seconds_+(provisional_?provisional_->audio.size()/16000.0:0.0);
        if (retained>options_.retained_limit-17) {
          lock.unlock(); stop("Retained audio reached its reserved stop threshold"); lock.lock();
          provisional_.reset(); cancel_provisional_=true;
          cv_.wait_for(lock,std::chrono::milliseconds(20));
          continue;
        }
      }
      if (stopping_ && !announced_stop) {
        std::string reason; { std::lock_guard<std::mutex> lock(mutex_); reason=stop_reason_; }
        emit({CaptionEvent::Kind::Status,0,0,0,"Stopping: "+reason}); announced_stop=true;
      }
      raw.clear();
      size_t frames=ring.pop(raw,chunk,&raw_end);
      if (!frames) {
        if (source_->finished() && ring.used_frames()==0) break;
        ring.wait([&]{ return ring.used_frames()>0 || source_->finished(); },std::chrono::milliseconds(20));
        continue;
      }
      raw_frames+=frames;
      mono.resize(frames);
      for (size_t i=0;i<frames;++i) { double sum=0; for (uint32_t c=0;c<channels;++c) sum+=raw[i*channels+c]; mono[i]=static_cast<float>(sum/channels); }
      handle(resampler.process(mono));
    }
    handle(resampler.process({},true));
    segmenter_->finish();
  } catch (const std::exception& e) {
    { std::lock_guard<std::mutex> lock(mutex_); error_=e.what(); }
    emit({CaptionEvent::Kind::Error,0,0,0,std::string("Preprocessing failed: ")+e.what()});
    if (source_) source_->stop();
  }
  while (!source_->finished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  { std::lock_guard<std::mutex> lock(mutex_); drained_=true; provisional_.reset(); segmenter_statistics_=segmenter_->statistics(); capture_statistics_=source_->statistics(); }
  cv_.notify_all();
}
void LivePipeline::work() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock,[&]{ return !finals_.empty() || (provisional_ && !suspended_) || drained_; });
    if (!finals_.empty()) {
      Final job=std::move(finals_.front()); finals_.pop_front();
      if (!pending_ends_.empty()) pending_ends_.pop_front();
      in_flight_end_=job.utterance.end_sample;
      double dur=job.utterance.audio.size()/16000.0;
      queued_seconds_=std::max(0.0,queued_seconds_-dur); in_flight_seconds_=dur;
      lock.unlock();
      CaptionEvent e; e.kind=CaptionEvent::Kind::Final; e.utterance=job.utterance.id; e.boundary=job.utterance.boundary;
      e.audio_start=job.utterance.start_sample/16000.0; e.audio_end=job.utterance.end_sample/16000.0;
      auto started=Clock::now();
      try {
        auto t=decode_(job.utterance.audio,nullptr,0);
        e.text=t.text; e.completion=t.completion; e.incomplete=t.completion!="eos"; e.detail=t.detail;
        if (e.detail.contains("cache_steps")) e.detail.erase("cache_steps");
      } catch (const std::exception& ex) {
        e.completion="error"; e.incomplete=true; e.status=ex.what();
        { std::lock_guard<std::mutex> guard(mutex_); error_=ex.what(); }
      }
      auto now=Clock::now();
      e.inference_seconds=std::chrono::duration<double>(now-started).count();
      e.lag_seconds=std::chrono::duration<double>(now-capture_time(job.utterance.end_sample)).count();
      e.since_start_seconds=std::chrono::duration<double>(now-capture_time(job.utterance.start_sample)).count();
      lock.lock();
      in_flight_seconds_=0; in_flight_end_.reset(); ++finals_done_; e.backlog_seconds=backlog_locked();
      review_backlog_locked(lock);
      lock.unlock();
      emit(e);
      if (e.completion=="error") stop("Inference failed");
      continue;
    }
    if (drained_) break;  // input drained and no finalized work left: stop after finals only
    if (provisional_ && !suspended_) {
      Provisional job=std::move(*provisional_); provisional_.reset();
      if (job.utterance<=last_final_queued_) { ++provisionals_skipped_; continue; }
      cancel_provisional_.store(false); // clear while holding the queue lock so a final cannot be missed
      in_flight_seconds_=job.audio.size()/16000.0;
      lock.unlock();
      CaptionEvent e; e.kind=CaptionEvent::Kind::Provisional; e.utterance=job.utterance; e.revision=job.revision;
      e.audio_start=job.start_sample/16000.0; e.audio_end=(job.start_sample+job.audio.size())/16000.0;
      auto started=Clock::now();
      bool deliver=true;
      try {
        auto t=decode_(job.audio,&cancel_provisional_,options_.provisional_max_tokens);
        e.text=t.text; e.completion=t.completion; e.incomplete=t.completion!="eos";
        if (t.completion=="cancelled") { deliver=false; }
      } catch (const std::exception& ex) { deliver=false; e.completion="error"; e.status=ex.what(); }
      auto now=Clock::now();
      e.inference_seconds=std::chrono::duration<double>(now-started).count();
      e.lag_seconds=std::chrono::duration<double>(now-capture_time(job.start_sample+job.audio.size())).count();
      e.since_start_seconds=std::chrono::duration<double>(now-capture_time(job.start_sample)).count();
      lock.lock();
      in_flight_seconds_=0;
      if (job.utterance<=last_final_queued_) deliver=false;  // a final for this utterance is already queued or done
      if (stopping_ || (provisional_ && provisional_->revision>job.revision)) deliver=false;
      if (deliver) ++provisionals_done_; else ++provisionals_cancelled_;
      e.backlog_seconds=backlog_locked();
      lock.unlock();
      if (deliver) emit(e);
      else if (e.completion=="error") emit({CaptionEvent::Kind::Error,0,job.utterance,job.revision,"Provisional decode failed: "+e.status});
      continue;
    }
    if (drained_) break;
  }
  running_=false;
  { std::lock_guard<std::mutex> lock(mutex_); source_.reset(); segmenter_.reset(); }
  CaptionEvent done; done.kind=CaptionEvent::Kind::Stopped;
  { std::lock_guard<std::mutex> lock(mutex_); done.status=stop_reason_.empty()?"Input exhausted":stop_reason_; }
  done.detail=statistics();
  emit(done);
}
Json LivePipeline::statistics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  FILETIME creation{},exit{},kernel{},user{};
  GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user);
  auto to_seconds=[](FILETIME f){ return (uint64_t(f.dwHighDateTime)<<32|f.dwLowDateTime)/1e7; };
  double wall=std::chrono::duration<double>(Clock::now()-started_at_).count();
  return {{"session",session_},{"stop_reason",stop_reason_},{"error",error_},{"audio_seconds_processed",position16k_.load()/16000.0},
          {"lag_clock","producer sample timestamps (WASAPI QPC for device capture)"},
          {"finals",finals_done_},{"provisionals",provisionals_done_},{"provisionals_cancelled",provisionals_cancelled_},{"provisionals_skipped",provisionals_skipped_},
          {"max_backlog_seconds",max_backlog_},{"backlog_definition","age of the oldest unfinished utterance since its audio ended (queued or decoding)"},{"max_queued_audio_seconds",max_queued_seconds_},{"retained_peak_seconds",retained_peak_},{"suspensions",suspensions_},
          {"wall_seconds",wall},{"process_cpu_seconds",to_seconds(kernel)+to_seconds(user)-cpu_at_start_},
          {"segmenter",segmenter_statistics_},{"capture",capture_statistics_.is_null()&&source_?source_->statistics():capture_statistics_},{"memory",memory_usage()}};
}
}
