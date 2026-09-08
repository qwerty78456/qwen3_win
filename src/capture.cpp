#include "capture.hpp"
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include <sstream>
namespace asrwin {
namespace {
struct PropVariantScope { PROPVARIANT value; PropVariantScope() { PropVariantInit(&value); } ~PropVariantScope() { PropVariantClear(&value); } };
std::wstring friendly_name(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ,&store))) return L"(unnamed device)";
  PropVariantScope name;
  if (FAILED(store->GetValue(PKEY_Device_FriendlyName,&name.value)) || name.value.vt!=VT_LPWSTR) return L"(unnamed device)";
  return name.value.pwszVal;
}
std::wstring device_identifier(IMMDevice* device) {
  LPWSTR id=nullptr; check(device->GetId(&id),"Read endpoint identifier");
  std::wstring result=id; CoTaskMemFree(id); return result;
}
ComPtr<IMMDeviceEnumerator> make_enumerator() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Create audio device enumerator");
  return enumerator;
}
// Forwards endpoint changes to the capture object. Callbacks arrive on a COM thread.
class Notifications : public IMMNotificationClient {
 public:
  Notifications(std::wstring id, bool watch_default, std::function<void(const std::wstring&)> lost):id_(std::move(id)),watch_default_(watch_default),lost_(std::move(lost)) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { auto n=--refs_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid==__uuidof(IUnknown) || riid==__uuidof(IMMNotificationClient)) { *out=static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
    *out=nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    if (id_==id && state!=DEVICE_STATE_ACTIVE) fire(L"Playback device became unavailable");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { if (id_==id) fire(L"Playback device was removed"); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
    if (watch_default_ && flow==eRender && role==eConsole && (!id || id_!=id)) fire(L"Default playback device changed");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  void fire(const std::wstring& reason) { bool expected=false; if (fired_.compare_exchange_strong(expected,true) && lost_) lost_(reason); }
  std::wstring id_; bool watch_default_; std::function<void(const std::wstring&)> lost_;
  std::atomic<ULONG> refs_{1}; std::atomic<bool> fired_{false};
};
}

std::vector<RenderDevice> render_devices() {
  auto enumerator=make_enumerator();
  std::wstring fallback;
  { ComPtr<IMMDevice> device; if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device))) fallback=device_identifier(device.Get()); }
  ComPtr<IMMDeviceCollection> collection;
  check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection),"Enumerate playback devices");
  UINT count=0; check(collection->GetCount(&count),"Count playback devices");
  std::vector<RenderDevice> result;
  for (UINT i=0;i<count;++i) {
    ComPtr<IMMDevice> device; check(collection->Item(i,&device),"Read playback device");
    RenderDevice entry{device_identifier(device.Get()),friendly_name(device.Get()),false};
    entry.is_default=entry.id==fallback;
    result.push_back(std::move(entry));
  }
  return result;
}
std::wstring default_render_device() {
  ComPtr<IMMDevice> device;
  check(make_enumerator()->GetDefaultAudioEndpoint(eRender,eConsole,&device),"Resolve default playback device");
  return device_identifier(device.Get());
}

RawRing::RawRing(uint32_t channels, uint32_t capacity_frames, uint32_t reserve_frames)
  :data_(size_t(channels)*capacity_frames),times_(capacity_frames),flags_(capacity_frames),formats_(capacity_frames),channels_(channels),capacity_(capacity_frames),reserve_(reserve_frames) {
  if (!channels || !capacity_frames || reserve_frames>=capacity_frames) throw std::runtime_error("Invalid capture ring geometry");
}
bool RawRing::push(const float* interleaved, uint32_t frames, Clock::time_point end_time, uint32_t rate, DWORD flags) {
  return push_bytes(reinterpret_cast<const BYTE*>(interleaved),frames,32,true,end_time,rate,flags);
}
bool RawRing::push_bytes(const BYTE* interleaved, uint32_t frames, uint32_t bits, bool floating, Clock::time_point end_time, uint32_t rate, DWORD flags) {
  size_t head=head_.load(std::memory_order_relaxed), tail=tail_.load(std::memory_order_acquire);
  if (head-tail+frames>capacity_) return false;
  for (uint32_t i=0;i<frames;++i) {
    size_t slot=((head+i)%capacity_)*channels_;
    if (!(flags&AUDCLNT_BUFFERFLAGS_SILENT)) memcpy(data_.data()+slot,interleaved+size_t(i)*channels_*(bits/8),channels_*(bits/8));
    times_[(head+i)%capacity_]=end_time-std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((frames-i-1)/double(rate)));
    flags_[(head+i)%capacity_]=flags;
    formats_[(head+i)%capacity_]=bits|(floating?0x100:0);
  }
  head_.store(head+frames,std::memory_order_release);
  size_t used=head+frames-tail, peak=peak_.load(std::memory_order_relaxed);
  while (used>peak && !peak_.compare_exchange_weak(peak,used)) {}
  cv_.notify_one();
  return true;
}
size_t RawRing::pop(std::vector<float>& out, size_t max_frames, Clock::time_point* end_time, DWORD* flags) {
  size_t head=head_.load(std::memory_order_acquire), tail=tail_.load(std::memory_order_relaxed);
  size_t frames=std::min(max_frames,head-tail);
  for (size_t i=0;i<frames;++i) {
    size_t slot=((tail+i)%capacity_)*channels_;
    uint32_t format=formats_[(tail+i)%capacity_], bits=format&0xff;
    const BYTE* data=reinterpret_cast<const BYTE*>(data_.data()+slot);
    for (uint32_t c=0;c<channels_;++c) {
      const BYTE* v=data+c*(bits/8);float sample=0;
      if (!(flags_[(tail+i)%capacity_]&AUDCLNT_BUFFERFLAGS_SILENT)) {
        if (format&0x100) memcpy(&sample,v,sizeof(sample));
        else if (bits==16) sample=static_cast<int16_t>(v[0]|v[1]<<8)/32768.f;
        else if (bits==24) { int32_t q=int32_t(v[0])|int32_t(v[1])<<8|int32_t(v[2])<<16;if(q&0x800000)q|=~0xffffff;sample=q/8388608.f; }
        else { int32_t value;memcpy(&value,v,4);sample=value/2147483648.f; }
      }
      out.push_back(sample);
    }
    if (flags) *flags|=flags_[(tail+i)%capacity_];
  }
  if (frames && end_time) *end_time=times_[(tail+frames-1)%capacity_];
  tail_.store(tail+frames,std::memory_order_release);
  return frames;
}

LoopbackCapture::LoopbackCapture(const std::wstring& device_id, CaptureEvents events):events_(std::move(events)) {
  enumerator_=make_enumerator();
  if (device_id.empty()) { capturing_default_=true; check(enumerator_->GetDefaultAudioEndpoint(eRender,eConsole,&device_),"Resolve default playback device"); }
  else check(enumerator_->GetDevice(device_id.c_str(),&device_),"Open selected playback device");
  DWORD state=0; check(device_->GetState(&state),"Read playback device state");
  if (state!=DEVICE_STATE_ACTIVE) throw std::runtime_error("Selected playback device is not active");
  id_=device_identifier(device_.Get()); name_=friendly_name(device_.Get());
  check(device_->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&capture_client_),"Activate loopback audio client");
  WAVEFORMATEX* mix=nullptr; check(capture_client_->GetMixFormat(&mix),"Read playback mix format");
  struct Free { WAVEFORMATEX* p; ~Free() { CoTaskMemFree(p); } } free_mix{mix};
  rate_=mix->nSamplesPerSec; channels_=mix->nChannels; bits_=mix->wBitsPerSample;
  if (mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
    auto* ext=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
    float_format_=ext->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    if (!float_format_ && ext->SubFormat!=KSDATAFORMAT_SUBTYPE_PCM) throw std::runtime_error("Unsupported playback mix format");
  } else if (mix->wFormatTag==WAVE_FORMAT_IEEE_FLOAT) float_format_=true;
  else if (mix->wFormatTag==WAVE_FORMAT_PCM) float_format_=false;
  else throw std::runtime_error("Unsupported playback mix format");
  if ((float_format_ && bits_!=32) || (!float_format_ && bits_!=16 && bits_!=24 && bits_!=32) || !channels_ || channels_>8 || rate_<8000 || rate_>192000)
    throw std::runtime_error("Unsupported playback mix format parameters");
  const REFERENCE_TIME hundred_ms=1000000;
  check(capture_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK,hundred_ms,0,mix,nullptr),"Initialize loopback capture");
  check(capture_client_->GetBufferSize(&buffer_frames_),"Read loopback buffer size");
  capture_event_=CreateEventW(nullptr,FALSE,FALSE,nullptr); render_event_=CreateEventW(nullptr,FALSE,FALSE,nullptr); stop_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  if (!capture_event_ || !render_event_ || !stop_event_) throw std::runtime_error("Cannot create capture events");
  check(capture_client_->SetEventHandle(capture_event_),"Attach loopback event");
  check(capture_client_->GetService(IID_PPV_ARGS(&capture_)),"Open loopback capture service");
  check(device_->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&render_client_),"Activate silent render client");
  check(render_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,hundred_ms,0,mix,nullptr),"Initialize silent render stream");
  check(render_client_->GetBufferSize(&render_buffer_frames_),"Read render buffer size");
  check(render_client_->SetEventHandle(render_event_),"Attach render event");
  check(render_client_->GetService(IID_PPV_ARGS(&render_)),"Open silent render service");
  // Two seconds of raw audio; one full device buffer stays reserved so the packet in hand always fits.
  ring_=std::make_unique<RawRing>(channels_,rate_*2,buffer_frames_);
  auto* watcher=new Notifications(id_,capturing_default_,[this](const std::wstring& reason){ if (events_.device_lost) events_.device_lost(reason); });
  notifications_.Attach(watcher);
  check(enumerator_->RegisterEndpointNotificationCallback(notifications_.Get()),"Watch playback devices");
}
LoopbackCapture::~LoopbackCapture() {
  stop();
  if (notifications_) enumerator_->UnregisterEndpointNotificationCallback(notifications_.Get());
  if (thread_.joinable()) thread_.join();
  for (HANDLE h:{capture_event_,render_event_,stop_event_}) if (h) CloseHandle(h);
}
void LoopbackCapture::start() {
  if (started_.exchange(true)) return;
  { BYTE* data=nullptr; if (SUCCEEDED(render_->GetBuffer(render_buffer_frames_,&data))) render_->ReleaseBuffer(render_buffer_frames_,AUDCLNT_BUFFERFLAGS_SILENT); }
  check(render_client_->Start(),"Start silent render stream");
  check(capture_client_->Start(),"Start loopback capture");
  thread_=std::thread([this]{ run(); });
}
void LoopbackCapture::stop() {
  if (!started_.load()) { finished_=true; return; }
  if (!stop_requested_.exchange(true)) SetEvent(stop_event_);
}
void LoopbackCapture::read_packets() {
  for (;;) {
    UINT32 next=0; HRESULT hr=capture_->GetNextPacketSize(&next);
    if (FAILED(hr)) { std::ostringstream m; m<<"Loopback packet query failed (device lost?) hr=0x"<<std::hex<<static_cast<unsigned long>(hr); throw std::runtime_error(m.str()); }
    if (!next) return;
    BYTE* data=nullptr; UINT32 frames=0; DWORD flags=0; UINT64 position=0, qpc=0;
    hr=capture_->GetBuffer(&data,&frames,&flags,&position,&qpc);
    if (hr==AUDCLNT_S_BUFFER_EMPTY) return;
    if (FAILED(hr)) { std::ostringstream m; m<<"Loopback buffer read failed (device lost?) hr=0x"<<std::hex<<static_cast<unsigned long>(hr); throw std::runtime_error(m.str()); }
    if (frames>buffer_frames_) {
      capture_->ReleaseBuffer(frames); dropped_frames_+=frames;
      throw std::runtime_error("Capture packet exceeds preallocated device capacity");
    }
    if (flags&AUDCLNT_BUFFERFLAGS_SILENT) ++silent_packets_;
    if (flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities_;
    if (flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestamp_errors_;
    if (have_position_) {
      if (position>expected_position_) device_gap_frames_+=position-expected_position_;
      else if (position<expected_position_) ++position_resets_;
    }
    expected_position_=position+frames; have_position_=true;
    auto packet_end=Clock::now();
    if (!(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
      LARGE_INTEGER counter{},frequency{}; QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
      double delta=qpc/1e7-counter.QuadPart/double(frequency.QuadPart)+frames/double(rate_);
      packet_end+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delta));
    }
    bool stored=ring_->push_bytes(data,frames,bits_,float_format_,packet_end,rate_,flags);
    check(capture_->ReleaseBuffer(frames),"Release loopback packet");
    ++packets_;
    if (!stored) { dropped_frames_+=frames; ++overflow_events_; stop_requested_=true; if (events_.overflow) events_.overflow(); return; }
    frames_+=frames;
    if (ring_->stop_threshold_reached()) { ++overflow_events_; stop_requested_=true; if (events_.overflow) events_.overflow(); return; }
  }
}
void LoopbackCapture::run() {
  ComScope com;
  DWORD task=0; HANDLE avrt=AvSetMmThreadCharacteristicsW(L"Audio",&task);
  try {
    HANDLE handles[]={stop_event_,capture_event_,render_event_};
    while (!stop_requested_.load()) {
      DWORD wait=WaitForMultipleObjects(3,handles,FALSE,20);  // 20 ms poll fallback in case events stall
      if (wait==WAIT_OBJECT_0) break;
      if (wait==WAIT_OBJECT_0+2) {
        UINT32 padding=0; if (SUCCEEDED(render_client_->GetCurrentPadding(&padding)) && padding<render_buffer_frames_) {
          UINT32 free_frames=render_buffer_frames_-padding; BYTE* data=nullptr;
          if (SUCCEEDED(render_->GetBuffer(free_frames,&data))) render_->ReleaseBuffer(free_frames,AUDCLNT_BUFFERFLAGS_SILENT);
        }
        continue;
      }
      read_packets();
    }
  } catch (const std::exception& e) {
    { std::lock_guard<std::mutex> lock(failure_mutex_); failure_=e.what(); }
    if (events_.failure) events_.failure(e.what());
  }
  capture_client_->Stop(); render_client_->Stop();
  if (avrt) AvRevertMmThreadCharacteristics(avrt);
  finished_=true;
  ring_->notify();
}
Json LoopbackCapture::statistics() const {
  std::string failure; { std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(failure_mutex_)); failure=failure_; }
  return {{"device",utf8(name_)},{"device_id",utf8(id_)},{"default_device",capturing_default_},{"sample_rate",rate_},{"channels",channels_},
          {"mix_format",float_format_?"float32":"pcm"+std::to_string(bits_)},{"device_buffer_frames",buffer_frames_},
          {"ring_capacity_frames",ring_?ring_->capacity_frames():0},{"ring_peak_used_frames",ring_?ring_->peak_used_frames():0},
          {"packets",packets_.load()},{"frames",frames_.load()},{"silent_packets",silent_packets_.load()},
          {"discontinuities",discontinuities_.load()},{"timestamp_errors",timestamp_errors_.load()},
          {"device_gap_frames",device_gap_frames_.load()},{"device_position_resets",position_resets_.load()},
          {"dropped_frames",dropped_frames_.load()},{"overflow_events",overflow_events_.load()},{"failure",failure}};
}
}
