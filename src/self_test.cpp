#include "self_test.hpp"
#include "pipeline.hpp"
#include "scoring.hpp"
#include <set>

namespace asrwin {
Json self_test(const fs::path& speech) {
  Json result={{"success",true},{"scope","Native queue, lifecycle and Unicode tests; synthetic decoder, no model correctness claim"},{"cases",Json::array()}};
  auto test=[&](const char* name,auto fn) {
    Json row={{"name",name},{"success",true}};
    try { fn(); } catch(const std::exception& e) { row["success"]=false; row["error"]=e.what(); result["success"]=false; }
    result["cases"].push_back(row);
  };
  auto require=[](bool condition,const char* message) { if (!condition) throw std::runtime_error(message); };
  test("ring wrap, timestamps, reserved capacity and rejected packet preserves accepted audio",[&]{
    RawRing ring(2,10,3); std::vector<float> a(16); for(size_t i=0;i<a.size();++i)a[i]=float(i);
    auto end=Clock::now(); require(ring.push(a.data(),7,end,16000,1),"initial packet"); require(ring.stop_threshold_reached(),"stop reserve");
    require(!ring.push(a.data(),4),"oversized packet accepted");
    std::vector<float> out; Clock::time_point time; DWORD flags=0;
    require(ring.pop(out,5,&time,&flags)==5,"first pop"); require(flags==1 && time<=end,"metadata");
    require(ring.push(a.data(),6,end,16000,4),"wrapped packet"); require(ring.pop(out,20,&time,&flags)==8,"drain");
    std::vector<float> expected(a.begin(),a.begin()+14);expected.insert(expected.end(),a.begin(),a.begin()+12);
    require(out==expected && ring.used_frames()==0 && time==end && flags==5,"samples or timing lost");
  });
  test("raw PCM packet conversion is deferred; silent packet can have no data",[&]{
    RawRing ring(1,10,3); const BYTE samples[]={0,0x80,0xff,0x7f};
    require(ring.push_bytes(samples,2,16,false,Clock::now(),16000,0),"PCM packet");
    require(ring.push_bytes(nullptr,2,32,true,Clock::now(),16000,AUDCLNT_BUFFERFLAGS_SILENT),"silent packet");
    std::vector<float> out;ring.pop(out,4);require(out.size()==4 && out[0]==-1.f && out[1]==32767/32768.f && out[2]==0 && out[3]==0,"PCM conversion");
  });
  test("Unicode roundtrip and mixed normalization",[&]{
    std::wstring text=L"中文 English 𠀀";require(wide(utf8(text))==text,"Unicode roundtrip");
    require(normalized_equal("中文，English!","中文 English"),"normalization");
  });
  auto audio=normalize_audio(read_wav(speech));
  test("stop, drain and restart the same pipeline without stale or duplicate finals",[&]{
    std::vector<CaptionEvent> events; std::mutex events_mutex;
    auto decode=[](std::span<const float>,const std::atomic<bool>* cancel,int){
      for(int i=0;i<30;++i) { if(cancel && *cancel) { Transcript t;t.completion="cancelled";return t; } std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      Transcript t;t.text="中文 English";t.completion="eos";return t;
    };
    LivePipeline pipeline(decode,[&](const CaptionEvent& e){std::lock_guard<std::mutex> lock(events_mutex);events.push_back(e);});
    for(uint64_t session=1;session<=3;++session) {
      pipeline.start(std::make_unique<FileSource>(audio,4,1),session);
      if(session==2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));auto start=Clock::now();pipeline.stop("Stop requested");
        require(seconds(start)<0.1,"Stop blocked its caller");
      }
      pipeline.wait(); require(!pipeline.running(),"did not stop");
      auto stats=pipeline.statistics();require(stats["error"]=="","pipeline error");require(stats["finals"].get<int>()>0,"accepted speech was not finalized");
      require(stats["retained_peak_seconds"].get<double>()<=60,"audio retention exceeded limit");
      std::set<uint64_t> finals;
      for(const auto& e:events) if(e.session==session) {
        if(e.kind==CaptionEvent::Kind::Final) require(finals.insert(e.utterance).second,"duplicate final");
        if(e.kind==CaptionEvent::Kind::Provisional) require(!finals.count(e.utterance),"stale provisional");
      }
      require(events.back().kind==CaptionEvent::Kind::Stopped && events.back().session==session,"terminal event missing");
      events.clear();
    }
  });
  test("forced 15 s splits preserve adjacent finalized audio",[&]{
    std::vector<Utterance> finals;
    Segmenter segmenter([&](Utterance&& u){finals.push_back(std::move(u));});
    for(int i=0;i<4;++i) segmenter.consume(audio.samples);
    segmenter.finish();segmenter.finish();
    require(!finals.empty(),"no speech segments");
    uint64_t end=0;bool forced=false;
    for(auto& u:finals) { require(u.start_sample>=end && u.audio.size()<=240000,"overlap or utterance limit exceeded");end=u.end_sample;forced|=u.boundary=="utterance_limit"; }
    require(forced,"fixture did not exercise forced splitting");
  });
  return result;
}
}
