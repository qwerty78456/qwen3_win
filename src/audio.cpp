#include "audio.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <bit>
namespace asrwin {
static uint16_t u16(const unsigned char* p){return static_cast<uint16_t>(p[0]|p[1]<<8);}
static uint32_t u32(const unsigned char* p){return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
Audio read_wav(const fs::path& path) {
 if(fs::file_size(path)>512ull*1024*1024) throw std::runtime_error("WAV exceeds 512 MiB benchmark limit");
 auto bytes=read_text(path);auto* p=reinterpret_cast<const unsigned char*>(bytes.data());size_t n=bytes.size();
 if(n<12 || memcmp(p,"RIFF",4) || memcmp(p+8,"WAVE",4))throw std::runtime_error("Expected RIFF PCM/float WAV");
 if(uint64_t(u32(p+4))+8!=n)throw std::runtime_error("RIFF length does not match WAV file length");
 uint16_t kind=0,channels=0,bits=0,align=0;uint32_t rate=0;std::span<const unsigned char> pcm;
 for(size_t pos=12;pos+8<=n;) {uint32_t len=u32(p+pos+4);if(len>n-pos-8)throw std::runtime_error("Truncated WAV chunk");auto* d=p+pos+8;
  if(!memcmp(p+pos,"fmt ",4)) {if(len<16||kind)throw std::runtime_error("Invalid or duplicate WAV format");kind=u16(d);channels=u16(d+2);rate=u32(d+4);align=u16(d+12);bits=u16(d+14);if(kind==0xfffe){const unsigned char tail[]={0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};if(len<40||u16(d+16)<22||memcmp(d+26,tail,14)||u16(d+18)!=bits)throw std::runtime_error("Unsupported extensible WAV subtype or valid-bit count");kind=u16(d+24);}}
  else if(!memcmp(p+pos,"data",4)){if(!pcm.empty())throw std::runtime_error("Multiple WAV data chunks unsupported");pcm={d,len};}
  pos+=8+size_t(len)+(len&1);
 }
 if(!channels || channels>8 || rate<8000 || rate>192000 || !align || align!=channels*(bits/8) || pcm.size()%align)throw std::runtime_error("Unsupported WAV sample format");
 if(!((kind==1&&(bits==8||bits==16||bits==24||bits==32))||(kind==3&&bits==32)))throw std::runtime_error("WAV must contain integer PCM or float32");
 Audio a;a.sample_rate=rate;a.samples.resize(pcm.size()/align);a.source={{"sample_rate",rate},{"channels",channels},{"bits",bits},{"format",kind==3?"float":"pcm"}};
 for(size_t i=0;i<a.samples.size();++i){double sum=0;for(int c=0;c<channels;++c){auto* v=pcm.data()+i*align+c*(bits/8);float x=0;
  if(kind==3){memcpy(&x,v,4);}else if(bits==8)x=(int(v[0])-128)/128.f;else if(bits==16)x=static_cast<int16_t>(u16(v))/32768.f;else if(bits==24){int32_t q=int32_t(v[0])|int32_t(v[1])<<8|int32_t(v[2])<<16;if(q&0x800000)q|=~0xffffff;x=q/8388608.f;}else x=std::bit_cast<int32_t>(u32(v))/2147483648.f;
  if(!std::isfinite(x))throw std::runtime_error("Non-finite audio sample");sum+=x;
 }a.samples[i]=static_cast<float>(sum/channels);}
 if(a.samples.empty())throw std::runtime_error("WAV contains no audio frames");return a;
}
Resampler::Resampler(uint32_t input_rate):rate_(input_rate){
 if(rate_==16000)return;
 check(MFStartup(MF_VERSION,MFSTARTUP_LITE),"Initialize Windows audio resampler");
 try {
 check(CoCreateInstance(CLSID_CResamplerMediaObject,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&transform_)),"Create Windows audio resampler");
 auto type=[](uint32_t hz){ComPtr<IMFMediaType> t;check(MFCreateMediaType(&t),"Create audio type");check(t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"Audio type");check(t->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_Float),"Float audio");check(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,1),"Mono audio");check(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,hz),"Audio rate");check(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,32),"Audio bits");check(t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4),"Audio alignment");check(t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,hz*4),"Audio byte rate");check(t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT,TRUE),"Audio independence");return t;};
 auto in=type(rate_),out=type(16000);check(transform_->SetInputType(0,in.Get(),0),"Set input rate");check(transform_->SetOutputType(0,out.Get(),0),"Set output rate");
 ComPtr<IWMResamplerProps> props;if(SUCCEEDED(transform_.As(&props)))check(props->SetHalfFilterLength(60),"Resampler quality");
 check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0),"Begin resampling");check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0),"Start resampling");
 }catch(...){MFShutdown();throw;}
}
Resampler::~Resampler(){if(rate_!=16000){transform_.Reset();MFShutdown();}}
void Resampler::drain(std::vector<float>& result){
 MFT_OUTPUT_STREAM_INFO info{};check(transform_->GetOutputStreamInfo(0,&info),"Resampler output format");
 for(;;){ComPtr<IMFMediaBuffer> buffer;check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize,65536),&buffer),"Resampler output allocation");ComPtr<IMFSample> sample;check(MFCreateSample(&sample),"Resampler output sample");check(sample->AddBuffer(buffer.Get()),"Resampler output buffer");
  MFT_OUTPUT_DATA_BUFFER out{};out.pSample=sample.Get();DWORD status=0;HRESULT hr=transform_->ProcessOutput(0,1,&out,&status);if(out.pEvents)out.pEvents->Release();if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT)break;check(hr,"Resample output");
  BYTE* data=nullptr;DWORD len=0;check(buffer->Lock(&data,nullptr,&len),"Read resampler output");size_t old=result.size();result.resize(old+len/4);memcpy(result.data()+old,data,len);buffer->Unlock();
 }
}
std::vector<float> Resampler::process(std::span<const float> input,bool finish){
 if(finished_)throw std::runtime_error("Resampler already finished");std::vector<float> result;
 if(rate_==16000){result.assign(input.begin(),input.end());finished_=finish;return result;}
 for(size_t offset=0;offset<input.size();){size_t count=std::min<size_t>(4096,input.size()-offset);ComPtr<IMFMediaBuffer> buffer;check(MFCreateMemoryBuffer(static_cast<DWORD>(count*4),&buffer),"Resampler input allocation");BYTE* dst=nullptr;check(buffer->Lock(&dst,nullptr,nullptr),"Resampler input lock");memcpy(dst,input.data()+offset,count*4);buffer->Unlock();check(buffer->SetCurrentLength(static_cast<DWORD>(count*4)),"Resampler input length");ComPtr<IMFSample> sample;check(MFCreateSample(&sample),"Resampler input sample");sample->AddBuffer(buffer.Get());sample->SetSampleTime(input_frames_*10000000/rate_);sample->SetSampleDuration(count*10000000/rate_);check(transform_->ProcessInput(0,sample.Get(),0),"Resample input");input_frames_+=count;offset+=count;drain(result);}
 if(finish){check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0),"End resampling");check(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0),"Drain resampler");drain(result);finished_=true;}return result;
}
Audio normalize_audio(Audio a){if(a.sample_rate!=16000){Resampler r(a.sample_rate);a.samples=r.process(a.samples,true);a.sample_rate=16000;}return a;}
}
