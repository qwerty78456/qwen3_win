#include "asr.hpp"
#include "audio.hpp"
#include "capture.hpp"
#include "diagnostics.hpp"
#include "gui.hpp"
#include "pipeline.hpp"
#include "scoring.hpp"
#include "segmenter.hpp"
#include "self_test.hpp"
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
using namespace asrwin;
namespace {
double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0;
  std::sort(values.begin(),values.end());
  size_t index=std::min(values.size()-1,static_cast<size_t>(std::ceil(p*values.size()))-(p>0?1:0));
  return values[index];
}
// Collects live pipeline events for the --simulate-live and --capture reports.
struct LiveCollector {
  std::mutex mutex; std::condition_variable cv; bool stopped=false; int errors=0, incomplete=0, duplicate_finals=0, stale_provisionals=0; uint64_t last_final_utterance=0; std::set<uint64_t> finalized;
  Clock::time_point t0=Clock::now();
  Json events=Json::array(); std::map<uint64_t,Json> utterances; Json stopped_detail; std::string transcript;
  void on(const CaptionEvent& e) {
    std::lock_guard<std::mutex> lock(mutex);
    const char* kind=e.kind==CaptionEvent::Kind::Status?"status":e.kind==CaptionEvent::Kind::Provisional?"provisional":e.kind==CaptionEvent::Kind::Final?"final":e.kind==CaptionEvent::Kind::Error?"error":"stopped";
    double t=seconds(t0);
    events.push_back({{"t",t},{"kind",kind},{"utterance",e.utterance},{"revision",e.revision},{"text",e.text},{"status",e.status},{"completion",e.completion},{"boundary",e.boundary},{"incomplete",e.incomplete},
                      {"audio_start",e.audio_start},{"audio_end",e.audio_end},{"lag_seconds",e.lag_seconds},{"since_start_seconds",e.since_start_seconds},{"backlog_seconds",e.backlog_seconds},{"inference_seconds",e.inference_seconds}});
    std::cout<<std::fixed<<std::setprecision(2)<<"["<<t<<"s] "<<kind;
    if (e.kind==CaptionEvent::Kind::Provisional || e.kind==CaptionEvent::Kind::Final) {
      auto& u=utterances[e.utterance];
      if (u.is_null()) u={{"id",e.utterance},{"audio_start",e.audio_start},{"provisional_lags",Json::array()},{"provisional_count",0}};
      if (e.kind==CaptionEvent::Kind::Provisional && (finalized.count(e.utterance) || e.utterance<=last_final_utterance)) ++stale_provisionals;  // would be discarded by a presenter
      if (e.kind==CaptionEvent::Kind::Final) { if (!finalized.insert(e.utterance).second) ++duplicate_finals; last_final_utterance=std::max(last_final_utterance,e.utterance); }
      if (e.kind==CaptionEvent::Kind::Provisional) { u["provisional_lags"].push_back(e.lag_seconds); u["provisional_count"]=u["provisional_count"].get<int>()+1; if (!u.contains("first_caption_lag")) u["first_caption_lag"]=e.since_start_seconds; }
      else { u["audio_end"]=e.audio_end; u["boundary"]=e.boundary; u["final_text"]=e.text; u["completion"]=e.completion; u["incomplete"]=e.incomplete; u["finalization_delay"]=e.lag_seconds; u["final_inference_seconds"]=e.inference_seconds;
             if (!u.contains("first_caption_lag") && !e.text.empty()) u["first_caption_lag"]=e.since_start_seconds;
             if (e.incomplete) ++incomplete; if (!e.text.empty()) transcript+=e.text+"\n"; }
      std::cout<<" #"<<e.utterance<<(e.kind==CaptionEvent::Kind::Provisional?" r"+std::to_string(e.revision):" "+e.boundary)<<" ["<<e.audio_start<<"-"<<e.audio_end<<"s] lag "<<e.lag_seconds<<"s backlog "<<e.backlog_seconds<<"s "<<e.completion<<": "<<e.text;
    } else { std::cout<<": "<<e.status; if (e.kind==CaptionEvent::Kind::Error) ++errors; }
    std::cout<<std::endl;
    if (e.kind==CaptionEvent::Kind::Stopped) { stopped=true; stopped_detail=e.detail; cv.notify_all(); }
  }
  void wait_stopped() { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock,[&]{ return stopped; }); }
  Json summary() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<double> provisional, first, finalization; Json rows=Json::array(); int finals=0, speech=0;
    for (auto& [id,u]:utterances) {
      rows.push_back(u);
      for (auto& v:u["provisional_lags"]) provisional.push_back(v.get<double>());
      if (u.contains("first_caption_lag")) { first.push_back(u["first_caption_lag"].get<double>()); ++speech; }
      if (u.contains("finalization_delay")) { finalization.push_back(u["finalization_delay"].get<double>()); ++finals; }
    }
    auto stats=[&](std::vector<double>& v){ return Json{{"count",v.size()},{"p50",percentile(v,0.5)},{"p95",percentile(v,0.95)},{"max",v.empty()?0:*std::max_element(v.begin(),v.end())}}; };
    std::string reason=stopped_detail.value("stop_reason","");
    bool clean_stop=reason.empty() || reason=="Stop requested" || reason=="Duration elapsed" || reason=="Input exhausted";
    Json capture=stopped_detail.value("capture",Json::object());
    bool no_loss=capture.value("dropped_frames",0)==0 && capture.value("overflow_events",0)==0;
    bool overload=reason.rfind("Overload",0)==0 || reason.rfind("Retained",0)==0;
    return {{"utterances",rows},{"final_count",finals},{"speech_utterances",speech},{"provisional_lag_seconds",stats(provisional)},{"first_caption_lag_seconds",stats(first)},
            {"finalization_delay_seconds",stats(finalization)},{"errors",errors},{"incomplete_finals",incomplete},{"duplicate_finals",duplicate_finals},{"stale_provisionals_delivered",stale_provisionals},{"overload_stop",overload},{"transcript",transcript},{"pipeline",stopped_detail},{"events",events},
            {"clean_stop",clean_stop},{"no_audio_loss",no_loss},{"success",errors==0 && incomplete==0 && duplicate_finals==0 && stale_provisionals==0 && clean_stop && no_loss && stopped_detail.value("error","").empty()}};
  }
};
std::wstring resolve_device(const std::wstring& selector) {
  if (selector.empty() || selector==L"default") return L"";
  for (auto& d:render_devices()) if (d.id==selector) return d.id;
  std::wstring needle=selector; std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
  for (auto& d:render_devices()) { std::wstring name=d.name; std::transform(name.begin(),name.end(),name.begin(),towlower); if (name.find(needle)!=std::wstring::npos) return d.id; }
  throw std::runtime_error("No active playback device matches: "+utf8(selector));
}
}

int wmain(int argc, wchar_t** argv) {
  fs::path report;
  try {
    SetConsoleOutputCP(CP_UTF8);
    fs::path default_model = executable_dir() / ASRWIN_MODEL_DIR;
    fs::path model = default_model, input, trace, verification, transcript_path;
    EngineOptions options; options.verify_distribution=true; PipelineOptions live;
    bool inspect=false, preprocess=false, tokenize=false, diagnostic=false, segment=false, encode=false, simulate=false, capture=false, devices=false, gui=false, gui_explicit=false, experimental=false, check_package=false;
    std::string token_text; std::wstring device_selector; double pace=1.0, duration=60, tail=1.0; int repeat=1; fs::path interleave,self_test_input; bool adapter_explicit=false;
    for (int i=1;i<argc;++i) {
      std::wstring arg=argv[i];
      auto value=[&]() -> std::wstring { if (++i>=argc) throw std::runtime_error("Missing argument value"); return argv[i]; };
      if (arg==L"--model") { model=value(); options.verify_distribution=false; }
      else if (arg==L"--self-test") self_test_input=value();
      else if (arg==L"--benchmark") input=value();
      else if (arg==L"--preprocess") { input=value(); preprocess=true; }
      else if (arg==L"--encode") { input=value(); preprocess=true; encode=true; }
      else if (arg==L"--segment") { input=value(); segment=true; }
      else if (arg==L"--tokenize") { token_text=utf8(value()); tokenize=true; }
      else if (arg==L"--verify") verification=value();
      else if (arg==L"--report") report=value();
      else if (arg==L"--trace") trace=value();
      else if (arg==L"--profile") options.profile=value();
      else if (arg==L"--threads") options.threads=std::stoi(value());
      else if (arg==L"--max-tokens") options.max_tokens=std::stoi(value());
      else if (arg==L"--inspect") inspect=true;
      else if (arg==L"--diagnostics") diagnostic=true;
      else if (arg==L"--devices") devices=true;
      else if (arg==L"--gui") { gui=true; gui_explicit=true; }
      else if (arg==L"--check-package") check_package=true;
      else if (arg==L"--simulate-live") simulate=true;
      else if (arg==L"--capture") capture=true;
      else if (arg==L"--device") device_selector=value();
      else if (arg==L"--duration") duration=std::stod(value());
      else if (arg==L"--pace") pace=std::stod(value());
      else if (arg==L"--tail-silence") tail=std::stod(value());
      else if (arg==L"--no-provisional") live.provisional=false;
      else if (arg==L"--provisional-max-tokens") live.provisional_max_tokens=std::stoi(value());
      else if (arg==L"--transcript") transcript_path=value();
      else if (arg==L"--repeat") repeat=std::stoi(value());
      else if (arg==L"--interleave") interleave=value();
      else if (arg==L"--experimental-directml") experimental=true;
      else if (arg==L"--memory-mode") options.memory_mode=utf8(value());
      else if (arg==L"--variant") options.variant=utf8(value());
      else if (arg==L"--skip-asset-check") options.verify_assets=false;  // development benchmarks only
      else if (arg==L"--slow-inference") options.slow_inference=std::stod(value());  // fault injection for live tests
      else if (arg==L"--adapter") { options.adapter=std::stoi(value()); adapter_explicit=true; }
      else if (arg==L"--provider") { auto p=value(); if (p==L"cpu") options.directml=false; else if (p==L"directml") options.directml=true; else throw std::runtime_error("Unknown provider"); }
      else if (arg==L"--help") {
        std::cout << "AsrWin — offline Mandarin/English playback captions (test candidate)\n"
          "  (no arguments)                       open the capture interface\n"
          "  --benchmark input.wav [--provider cpu] [--report results.json] [--trace dir]\n"
          "  --benchmark input.wav --simulate-live [--pace 1.0] [--no-provisional] --report live.json\n"
          "  --capture [--device default|name|id] --duration 60 [--report live.json] [--transcript out.txt]\n"
          "  --verify regression\\manifest.json --report verification.json [--trace dir]\n"
          "  --devices | --inspect | --diagnostics | --preprocess input.wav | --segment input.wav | --tokenize text\n"
          "Options: --model dir  --threads 4  --max-tokens 512  --variant fp32|int4  --provider cpu|directml\n";
        return 0;
      } else throw std::runtime_error("Unknown argument: "+utf8(arg));
    }
    bool action = !self_test_input.empty() || !input.empty() || !verification.empty() || inspect || diagnostic || devices || tokenize || preprocess || segment || check_package || capture || simulate;
    int modes=int(!self_test_input.empty())+int(!input.empty())+int(!verification.empty())+int(inspect)+int(diagnostic)+int(devices)+int(tokenize)+int(check_package)+int(capture);
    if (modes>1 || (gui_explicit && action) || (simulate && input.empty())) throw std::runtime_error("Select exactly one action; --simulate-live requires --benchmark input.wav");
    if (gui_explicit || !action) gui = true;
    if (options.threads<1 || options.threads>16 || options.max_tokens<1 || options.max_tokens>1024) throw std::runtime_error("Invalid thread/token limit");
    if (!std::isfinite(duration) || duration<=0 || duration>86400 || !std::isfinite(pace) || pace<0 || pace>100 ||
        !std::isfinite(tail) || tail<0 || tail>60 || !std::isfinite(options.slow_inference) || options.slow_inference<0 || options.slow_inference>300 ||
        live.provisional_max_tokens<1 || live.provisional_max_tokens>1024 || options.adapter<0)
      throw std::runtime_error("Invalid duration, replay pace, tail, inference delay, provisional token limit or adapter");
    if (options.directml && (!experimental || !adapter_explicit || gui))
      throw std::runtime_error("DirectML is unvalidated; use --experimental-directml with an explicit adapter for command-line experiments. The interface uses CPU until GPU validation is complete.");
    if (gui) {
      DWORD owners[2]; if (GetConsoleProcessList(owners,2)<=1) FreeConsole();  // launched from Explorer: no console needed
      return run_gui(GetModuleHandleW(nullptr),GuiOptions{model,options.threads,options.max_tokens,options.variant,options.directml,options.verify_distribution});
    }
    ComScope com;
    if (!self_test_input.empty()) { auto result=self_test(self_test_input); if (!report.empty()) write_json(report,result); std::cout<<result.dump(2)<<'\n'; return result["success"].get<bool>()?0:2; }
    if (check_package) {
      auto checked=verify_package(executable_dir());
      if (!report.empty()) write_json(report,checked);
      std::cout<<"Package OK: "<<checked["verified_files"]<<" verified files\n";
      return 0;
    }
    if (diagnostic) { auto result=diagnostics(); std::cout<<result.dump(2)<<'\n'; if (!report.empty()) write_json(report,result); return 0; }
    if (devices) {
      Json result=Json::array();
      for (auto& d:render_devices()) result.push_back({{"id",utf8(d.id)},{"name",utf8(d.name)},{"default",d.is_default}});
      std::cout<<result.dump(2)<<'\n'; if (!report.empty()) write_json(report,result); return 0;
    }
    if (tokenize) {
      auto tokenizer=tokenizers::Tokenizer::FromBlobJSON(read_text(model/"tokenizer.json"));
      auto ids=tokenizer->Encode(token_text);
      Json result={{"text",token_text},{"ids",ids},{"decoded",tokenizer->Decode(ids)}};
      std::cout<<result.dump(2)<<'\n'; if (!report.empty()) write_json(report,result); return 0;
    }
    if (preprocess) {
      auto audio=normalize_audio(read_wav(input));
      audio.samples=model_samples(audio.samples);
      MelExtractor extractor(model/"mel_filters.bin");
      auto mel=extractor.compute(audio.samples);
      Json result={{"source",audio.source},{"samples",audio.samples.size()},{"frames",mel.frames},{"audio_tokens",MelExtractor::audio_tokens(mel.frames)}};
      if (!trace.empty()) { write_floats(trace/"pcm.f32",audio.samples); write_floats(trace/"mel.f32",mel.values); }
      if (encode) {
        result["scope"]="encoder diagnostic only; does not prove complete transcription";
        result["encoder_sha256"]=sha256_file(model/"encoder.onnx");
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"AsrWin-encoder-proof");
        Ort::SessionOptions settings; settings.SetIntraOpNumThreads(options.threads); settings.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL); settings.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC); settings.AddConfigEntry("session.intra_op.allow_spinning","0");
        auto started=Clock::now(); Ort::Session encoder(env,(model/"encoder.onnx").c_str(),settings); result["load_seconds"]=seconds(started);
        auto cpu=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        int64_t shape[]={1,128,mel.frames};
        auto tensor=Ort::Value::CreateTensor<float>(cpu,mel.values.data(),mel.values.size(),shape,3);
        const char* in[]={"mel"}; const char* out[]={"audio_features"};
        started=Clock::now(); auto outputs=encoder.Run(Ort::RunOptions{nullptr},in,&tensor,1,out,1); result["encoder_seconds"]=seconds(started);
        auto info=outputs[0].GetTensorTypeAndShapeInfo(); result["shape"]=info.GetShape(); result["memory"]=memory_usage();
        if (!trace.empty()) write_floats(trace/"encoder.f32",{outputs[0].GetTensorData<float>(),info.GetElementCount()});
      }
      if (!report.empty()) write_json(report,result);
      std::cout<<result.dump(2)<<'\n'; return 0;
    }
    if (segment) {
      auto audio=normalize_audio(read_wav(input));
      Json result={{"segments",Json::array()},{"source_samples",audio.samples.size()},{"vad_mode",2},{"frame_ms",20},{"start_voiced_ms",100},{"minimum_rms",0.0001}};
      Segmenter detector([&](Utterance&& u){
        result["segments"].push_back({{"id",u.id},{"start_sample",u.start_sample},{"end_sample",u.end_sample},{"samples",u.audio.size()},{"boundary",u.boundary}});
        if (!trace.empty()) write_floats(trace/(std::to_string(u.id)+".f32"),u.audio);
      });
      detector.consume(audio.samples); detector.finish();
      result["detector"]=detector.statistics();
      if (!report.empty()) write_json(report,result);
      std::cout<<result.dump(2)<<'\n'; return 0;
    }
    if (input.empty() && verification.empty() && !inspect && !capture) { std::cout<<"Run --help for commands, or start without arguments for the interface.\n"; return 0; }
    auto load=Clock::now();
    options.progress=[](const std::string& m){ std::cerr<<m<<"...\n"; };
    Engine engine(model,options);
    Json result={{"engine",engine.inspect()},{"model_load_seconds",seconds(load)},{"memory_after_load",memory_usage()}};
    if (!verification.empty()) {
      auto manifest=Json::parse(read_text(verification));
      auto& fixtures=manifest.at("fixtures");
      if (fixtures.size()!=24 || manifest.at("reference_status")!="complete") throw std::runtime_error("Verification requires 24 frozen fixtures with complete official-model references");
      std::set<std::string> fixture_ids; std::map<std::string,int> categories;
      for (const auto& fixture:fixtures) {
        if (!fixture_ids.insert(fixture.at("id").get<std::string>()).second) throw std::runtime_error("Duplicate regression fixture ID");
        ++categories[fixture.at("category").get<std::string>()];
      }
      if (categories!=std::map<std::string,int>{{"mandarin",6},{"english",6},{"mixed",6},{"non-speech",6}})
        throw std::runtime_error("Regression requires six fixtures in each language/non-speech category");
      result["fixtures"]=Json::array(); bool success=true;
      for (const auto& fixture:fixtures) {
        auto id=fixture.at("id").get<std::string>();
        Json row={{"id",id},{"success",false}};
        try {
          auto wav=verification.parent_path()/wide(fixture.at("path").get<std::string>());
          if (sha256_file(wav)!=fixture.at("sha256")) throw std::runtime_error("Fixture hash mismatch");
          auto refpath=verification.parent_path()/wide(fixture.at("reference_path").get<std::string>());
          if (sha256_file(refpath)!=fixture.at("reference_sha256")) throw std::runtime_error("Reference hash mismatch");
          auto reference=Json::parse(read_text(refpath));
          if (reference.at("reference_model_repo")!=result["engine"]["reference_model"]["repo"] || reference.at("reference_model_revision")!=result["engine"]["reference_model"]["revision"])
            throw std::runtime_error("Reference belongs to a different model revision");
          auto audio=normalize_audio(read_wav(wav));
          auto observed=engine.transcribe(audio.samples,trace.empty()?fs::path{}:trace/wide(id));
          row["observed"]=observed.detail; row["reference"]=reference;
          row["normalized_transcript_match"]=normalized_equal(observed.text,reference.at("text").get<std::string>());
          row["tokens_exact"]=observed.tokens==reference.at("tokens").get<std::vector<int32_t>>();
          row["human_error"]=error_rate(fixture.at("human_text").get<std::string>(),observed.text,fixture.at("category")=="mandarin");
          row["success"]=observed.completion=="eos" && reference.at("completion")=="eos" && row["normalized_transcript_match"].get<bool>() && row["tokens_exact"].get<bool>() && (fixture.at("category")!="non-speech" || observed.text.empty());
        } catch (const std::exception& e) { row["error"]=e.what(); }
        success=success && row["success"].get<bool>();
        std::cout<<id<<": "<<(row["success"].get<bool>()?"PASS":"FAIL")<<std::endl;
        result["fixtures"].push_back(std::move(row));
        result["success"]=success;
        result["proof_complete"]=false;  // numerical stage comparison, silence gate and boundary tests are separate proof evidence
        if (!report.empty()) write_json(report,result);
      }
      result["profiles"]=engine.finish_profiling();
      if (!report.empty()) write_json(report,result);
      return success?0:2;
    }
    if (simulate || capture) {
      LiveCollector collector;
      LivePipeline pipeline(engine,[&](const CaptionEvent& e){ collector.on(e); },live);
      std::unique_ptr<AudioSource> source;
      Json source_info;
      if (simulate) { auto audio=read_wav(input); source_info={{"file",utf8(input.wstring())},{"source",audio.source},{"audio_seconds",audio.samples.size()/double(audio.sample_rate)},{"pace",pace}}; source=std::make_unique<FileSource>(std::move(audio),pace,tail); }
      else {
        auto* p=&pipeline;
        CaptureEvents events{[p](const std::wstring& r){ p->stop(utf8(r)); },[p]{ p->stop("Capture buffer reached its stop threshold"); },[p](const std::string& e){ p->stop("Capture failed: "+e); }};
        auto loopback=std::make_unique<LoopbackCapture>(resolve_device(device_selector),events);
        source_info={{"device",utf8(loopback->name())},{"device_id",utf8(loopback->id())},{"default_device",loopback->capturing_default()},{"sample_rate",loopback->sample_rate()},{"channels",loopback->channels()},{"duration_seconds",duration}};
        source=std::move(loopback);
      }
      pipeline.start(std::move(source),1);
      if (capture) std::cout<<"Capturing playback from: "<<source_info["device"].get<std::string>()<<" for "<<duration<<" s"<<std::endl;
      if (capture) {
        std::unique_lock<std::mutex> lock(collector.mutex);
        collector.cv.wait_for(lock,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(duration)),[&]{ return collector.stopped; });
        lock.unlock();
        pipeline.stop("Duration elapsed");
      }
      collector.wait_stopped(); pipeline.wait();
      result["mode"]=simulate?"simulate-live":"capture"; result["source"]=source_info; result["live"]=collector.summary(); result["options"]={{"provisional",live.provisional},{"provisional_after",live.provisional_after},{"provisional_interval",live.provisional_interval},{"suspend_backlog",live.suspend_backlog},{"resume_backlog",live.resume_backlog},{"overload_backlog",live.overload_backlog},{"retained_limit",live.retained_limit},{"slow_inference",options.slow_inference},{"threads",options.threads},{"variant",options.variant}};
      result["success"]=result["live"]["success"];
      if (!transcript_path.empty()) { std::ofstream f(transcript_path,std::ios::binary); f.write("\xEF\xBB\xBF",3); auto text=result["live"]["transcript"].get<std::string>(); f.write(text.data(),text.size()); if (!f) throw std::runtime_error("Cannot write transcript: "+utf8(transcript_path.wstring())); }
      if (!report.empty()) write_json(report,result);
      auto& s=result["live"];
      std::cout<<"finals "<<s["final_count"]<<", provisional lag p95 "<<s["provisional_lag_seconds"]["p95"]<<" s, first-caption p95 "<<s["first_caption_lag_seconds"]["p95"]<<" s, finalization p95 "<<s["finalization_delay_seconds"]["p95"]<<" s, max backlog "<<s["pipeline"]["max_backlog_seconds"]<<" s, peak working set "<<s["pipeline"]["memory"]["peak_working_set_bytes"]<<" bytes\n";
      return result["success"].get<bool>()?0:2;
    }
    if (!input.empty()) {
      auto start=Clock::now();
      auto audio=normalize_audio(read_wav(input));
      double conversion=seconds(start);
      if (repeat<1 || repeat>100) throw std::runtime_error("Invalid repeat count");
      std::optional<Audio> other; if (!interleave.empty()) other=normalize_audio(read_wav(interleave));
      auto transcription=engine.transcribe(audio.samples,trace);
      Json runs=Json::array(); bool identical=true; std::vector<int32_t> first_other;
      auto record=[&](const char* file,int run,const Transcript& t){ runs.push_back({{"file",file},{"run",run},{"tokens",t.tokens},{"text",t.text},{"completion",t.completion},{"total_seconds",t.detail.at("total_seconds")}}); };
      record("input",0,transcription);
      for (int r=1;r<repeat;++r) {
        if (other) { auto t2=engine.transcribe(other->samples); if (first_other.empty()) first_other=t2.tokens; else identical=identical && t2.tokens==first_other; record("interleave",r,t2); }
        auto again=engine.transcribe(audio.samples); identical=identical && again.tokens==transcription.tokens; record("input",r,again);
      }
      if (repeat>1) { result["repeat_runs"]=runs; result["repeat_tokens_identical"]=identical; }
      result["transcription"]=transcription.detail; result["audio_conversion_seconds"]=conversion;
      result["total_elapsed_seconds"]=result["model_load_seconds"].get<double>()+conversion+transcription.detail.at("total_seconds").get<double>();
      result["source"]=audio.source; result["file"]=utf8(input.wstring()); result["success"]=transcription.completion=="eos" && identical;
      result["profiles"]=engine.finish_profiling();
      std::cout<<transcription.text<<'\n';
      if (!report.empty()) write_json(report,result);
      return result["success"].get<bool>()?0:2;
    }
    std::cout<<result.dump(2)<<'\n';
    if (!report.empty()) write_json(report,result);
    return 0;
  } catch (const std::exception& e) {
    std::cerr<<"Error: "<<e.what()<<'\n';
    if (!report.empty()) { try { write_json(report,{{"success",false},{"error",e.what()}}); } catch (...) {} }
    return 1;
  }
}
