#pragma once
#include "asr.hpp"
#include "audio.hpp"
#include "capture.hpp"
#include "segmenter.hpp"
#include <deque>
#include <map>
namespace asrwin {
struct CaptionEvent {
  enum class Kind { Status, Provisional, Final, Stopped, Error } kind=Kind::Status;
  uint64_t session=0, utterance=0, revision=0;
  std::string status;                 // status text or error message
  std::string text;                   // caption text (provisional or final)
  std::string completion;             // eos | token_limit | cancelled | error
  std::string boundary;               // final only: silence | utterance_limit | stop
  bool incomplete=false;              // final that must not be presented as a successful transcript
  double audio_start=0, audio_end=0;  // seconds on the 16 kHz session timeline
  double lag_seconds=0;               // wall time from capture of audio_end to emission
  double since_start_seconds=0;       // wall time from capture of audio_start to emission
  double backlog_seconds=0;           // finalized-work backlog when emitted
  double inference_seconds=0;
  Json detail;
};
struct PipelineOptions {
  double provisional_after=2.0;       // first provisional request after this much utterance audio
  double provisional_interval=2.0;    // then at most every N seconds of new audio
  double suspend_backlog=4.0, resume_backlog=2.0, overload_backlog=20.0, retained_limit=60.0;
  int provisional_max_tokens=256;
  bool provisional=true;
};
// Replays a WAV through the same raw ring, packetized like a device, paced in real time.
class FileSource : public AudioSource {
 public:
  FileSource(Audio audio, double pace=1.0, double tail_silence=1.0);
  ~FileSource() override;
  uint32_t sample_rate() const override { return audio_.sample_rate; }
  uint32_t channels() const override { return 1; }
  RawRing& ring() override { return ring_; }
  void start() override;
  void stop() override;
  bool finished() const override { return finished_.load(); }
  std::wstring name() const override { return L"WAV replay"; }
  Json statistics() const override;
 private:
  Audio audio_; double pace_, tail_; RawRing ring_;
  std::thread thread_; std::atomic<bool> stop_{false}, finished_{false}, started_{false};
  std::atomic<uint64_t> packets_{0}, frames_{0}, late_packets_{0}; std::atomic<int> overflow_{0};
  std::atomic<double> max_late_{0};
};
class LivePipeline {
 public:
  using Sink=std::function<void(const CaptionEvent&)>;
  using Decode=std::function<Transcript(std::span<const float>,const std::atomic<bool>*,int)>;
  LivePipeline(Engine& engine, Sink sink, PipelineOptions options={});
  LivePipeline(Decode decode, Sink sink, PipelineOptions options={});
  ~LivePipeline();
  LivePipeline(const LivePipeline&)=delete;
  LivePipeline& operator=(const LivePipeline&)=delete;
  uint64_t start(std::unique_ptr<AudioSource> source, uint64_t session);
  void stop(const std::string& reason);   // asynchronous: capture stops now, Stopped arrives after drain
  bool running() const { return running_.load(); }
  void wait();                            // join worker threads (after Stopped or from any non-worker thread)
  Json statistics() const;
  AudioSource* source() { return source_.get(); }
 private:
  struct Final { Utterance utterance; };
  struct Provisional { uint64_t utterance=0, revision=0, start_sample=0; std::vector<float> audio; };
  void preprocess();
  void work();
  void emit(CaptionEvent e);
  void record_timeline(uint64_t sample16k, Clock::time_point time);
  Clock::time_point capture_time(uint64_t sample16k) const;
  // Finalized-work backlog = how long the oldest unfinished utterance has been waiting since its audio ended
  // (queued or being decoded). Independent of utterance length; grows while finalization falls behind.
  double backlog_locked() const;
  void review_backlog_locked(std::unique_lock<std::mutex>& lock);
  Decode decode_; Sink sink_; PipelineOptions options_;
  std::unique_ptr<AudioSource> source_;
  std::thread preprocess_, worker_;
  mutable std::mutex mutex_; std::condition_variable cv_;
  std::deque<Final> finals_; std::optional<Provisional> provisional_;
  std::deque<uint64_t> pending_ends_; std::optional<uint64_t> in_flight_end_;  // end samples of queued / decoding finals
  double queued_seconds_=0, in_flight_seconds_=0, max_backlog_=0, max_queued_seconds_=0, retained_peak_=0;
  bool suspended_=false, drained_=false;
  uint64_t last_final_queued_=0, revision_=0, session_=0;
  std::map<uint64_t,uint64_t> provisional_progress_;  // utterance -> samples at last provisional request
  std::atomic<bool> running_{false}, stopping_{false}, cancel_provisional_{false};
  std::string stop_reason_; std::string error_;
  mutable std::mutex timeline_mutex_; std::deque<std::pair<uint64_t,Clock::time_point>> timeline_;
  std::mutex sink_mutex_;
  std::atomic<uint64_t> position16k_{0};
  uint64_t finals_done_=0, provisionals_done_=0, provisionals_cancelled_=0, provisionals_skipped_=0, suspensions_=0;
  Json segmenter_statistics_;
  Clock::time_point started_at_;
  double cpu_at_start_=0;
  std::unique_ptr<Segmenter> segmenter_;
  Json capture_statistics_;
};
}
