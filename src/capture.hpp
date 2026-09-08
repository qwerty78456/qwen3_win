#pragma once
#include "common.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <mmdeviceapi.h>
#include <audioclient.h>
namespace asrwin {
struct RenderDevice { std::wstring id, name; bool is_default=false; };
std::vector<RenderDevice> render_devices();
std::wstring default_render_device();

// Single-producer / single-consumer ring of interleaved float frames. The capture
// thread only copies into it; it never blocks and never overwrites accepted audio.
class RawRing {
 public:
  RawRing(uint32_t channels, uint32_t capacity_frames, uint32_t reserve_frames);
  bool push(const float* interleaved, uint32_t frames, Clock::time_point end_time=Clock::now(), uint32_t rate=16000, DWORD flags=0);
  bool push_bytes(const BYTE* interleaved, uint32_t frames, uint32_t bits, bool floating, Clock::time_point end_time, uint32_t rate, DWORD flags);
  size_t pop(std::vector<float>& out, size_t max_frames, Clock::time_point* end_time=nullptr, DWORD* flags=nullptr);
  bool stop_threshold_reached() const { return used_frames()>=capacity_-reserve_; }
  size_t used_frames() const { return head_.load(std::memory_order_acquire)-tail_.load(std::memory_order_acquire); }
  uint32_t channels() const { return channels_; }
  uint32_t capacity_frames() const { return capacity_; }
  uint32_t reserve_frames() const { return reserve_; }
  size_t peak_used_frames() const { return peak_.load(); }
  void notify() { cv_.notify_one(); }
  template <class Pred> bool wait(Pred ready, std::chrono::milliseconds timeout) { std::unique_lock<std::mutex> lock(mutex_); return cv_.wait_for(lock,timeout,ready); }
 private:
  std::vector<float> data_;
  std::vector<Clock::time_point> times_;
  std::vector<DWORD> flags_;
  std::vector<uint32_t> formats_;
  uint32_t channels_, capacity_, reserve_;
  std::atomic<size_t> head_{0}, tail_{0}, peak_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
};

// Any audio producer feeding the live pipeline: WASAPI loopback or a paced WAV file.
class AudioSource {
 public:
  virtual ~AudioSource()=default;
  virtual uint32_t sample_rate() const=0;
  virtual uint32_t channels() const=0;
  virtual RawRing& ring()=0;
  virtual void start()=0;
  virtual void stop()=0;                 // stop producing; must be idempotent and non-blocking for the caller's UI
  virtual bool finished() const=0;       // no more frames will be pushed
  virtual std::wstring name() const=0;
  virtual Json statistics() const=0;
};

struct CaptureEvents {
  std::function<void(const std::wstring&)> device_lost;  // removed, disabled, or default changed
  std::function<void()> overflow;                        // ring reached its stop threshold
  std::function<void(const std::string&)> failure;       // WASAPI failure on the capture thread
};

// Event-driven shared-mode WASAPI loopback of one render endpoint. A silent render
// stream on the same endpoint keeps the engine pumping so loopback delivers continuous
// packets (silence-flagged when nothing plays).
class LoopbackCapture : public AudioSource {
 public:
  LoopbackCapture(const std::wstring& device_id, CaptureEvents events);  // empty id = resolve default now
  ~LoopbackCapture() override;
  uint32_t sample_rate() const override { return rate_; }
  uint32_t channels() const override { return channels_; }
  RawRing& ring() override { return *ring_; }
  void start() override;
  void stop() override;
  bool finished() const override { return finished_.load(); }
  std::wstring name() const override { return name_; }
  std::wstring id() const { return id_; }
  bool capturing_default() const { return capturing_default_; }
  Json statistics() const override;
 private:
  void run();
  void read_packets();
  CaptureEvents events_;
  ComPtr<IMMDeviceEnumerator> enumerator_;
  ComPtr<IMMDevice> device_;
  ComPtr<IAudioClient> capture_client_, render_client_;
  ComPtr<IAudioCaptureClient> capture_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IMMNotificationClient> notifications_;
  HANDLE capture_event_=nullptr, render_event_=nullptr, stop_event_=nullptr;
  std::unique_ptr<RawRing> ring_;
  std::thread thread_;
  std::wstring id_, name_;
  bool capturing_default_=false;
  uint32_t rate_=0, channels_=0, bits_=0, buffer_frames_=0, render_buffer_frames_=0;
  bool float_format_=true;
  std::atomic<bool> stop_requested_{false}, finished_{false}, started_{false};
  std::atomic<uint64_t> packets_{0}, frames_{0}, discontinuities_{0}, silent_packets_{0}, timestamp_errors_{0}, dropped_frames_{0};
  std::atomic<int> overflow_events_{0};
  uint64_t expected_position_=0;
  bool have_position_=false;
  std::atomic<uint64_t> device_gap_frames_{0}, position_resets_{0};
  std::string failure_;
  std::mutex failure_mutex_;
};
}
