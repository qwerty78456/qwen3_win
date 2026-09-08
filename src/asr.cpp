#include "asr.hpp"
#include <dml_provider_factory.h>
#include <numeric>
#include <thread>
namespace asrwin {
Engine::Engine(const fs::path& model, EngineOptions options):model_(model),options_(std::move(options)) {
  if (options_.verify_distribution) { report("Verifying portable package"); verify_package(executable_dir()); }
  if (options_.variant!="fp32" && options_.variant!="int4") throw std::runtime_error("Unknown model variant");
  if (options_.verify_assets) { report("Verifying model assets"); verify_files(model,options_.variant); }
  const std::string suffix=options_.variant=="int4"?".int4.onnx":".onnx";
  config_=Json::parse(read_text(model/"config.json"));
  hidden_=config_.at("decoder").at("hidden_size");
  vocab_=config_.at("decoder").at("vocab_size");
  auto dtype=config_.at("embed_tokens_dtype").get<std::string>();
  if (dtype!="float16" && dtype!="float32") throw std::runtime_error("Unsupported embedding storage format");
  half_=dtype=="float16";
  embeddings_=std::make_unique<MappedFile>(model/"embed_tokens.bin");
  if (embeddings_->size()!=size_t(hidden_)*vocab_*(half_?2:4)) throw std::runtime_error("Embedding dimensions do not match asset size");
  tokenizer_=tokenizers::Tokenizer::FromBlobJSON(read_text(model/"tokenizer.json"));
  if (!tokenizer_) throw std::runtime_error("Cannot load tokenizer");
  auto prompt=Json::parse(read_text(model/"prompt_reference.json"));
  prefix_=tokenizer_->Encode(prompt.at("prefix").get<std::string>());
  suffix_=tokenizer_->Encode(prompt.at("suffix").get<std::string>());
  if (prefix_.empty() || suffix_.empty()) throw std::runtime_error("Empty model prompt");
  for (auto& test:prompt.at("cases"))
    if (tokenizer_->Encode(test.at("text").get<std::string>())!=test.at("ids").get<std::vector<int32_t>>())
      throw std::runtime_error("Native tokenizer differs from official tokenizer reference");
  eos_=config_.at("special_tokens").at("eos_token_ids").get<std::vector<int32_t>>();
  for (auto pair:std::vector<std::pair<std::string,std::string>>{{"audio_pad_token_id","<|audio_pad|>"},{"asr_text_token_id","<asr_text>"},{"im_end_token_id","<|im_end|>"}})
    if (tokenizer_->TokenToId(pair.second)!=config_.at("special_tokens").at(pair.first).get<int>()) throw std::runtime_error("Tokenizer special-token mismatch");
  mel_=std::make_unique<MelExtractor>(model/"mel_filters.bin");
  Ort::SessionOptions so;
  so.SetIntraOpNumThreads(options_.threads);
  so.SetInterOpNumThreads(1);
  so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  so.AddConfigEntry("session.intra_op.allow_spinning","0");
  if (!options_.profile.empty()) { fs::create_directories(options_.profile); so.EnableProfiling((options_.profile/"encoder").c_str()); }
  if (options_.directml) { so.DisableMemPattern(); Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so,options_.adapter)); }
  if (options_.memory_mode!="default" && options_.memory_mode!="no-prepack" && options_.memory_mode!="shared-prepack") throw std::runtime_error("Unknown memory mode");
  report("Loading encoder"); encoder_=std::make_unique<Ort::Session>(env_,(model/"encoder.onnx").c_str(),so);
  // Both decoder graphs reference the same external weights. Sharing one pre-packed weights container
  // keeps a single packed copy for MatMul kernels instead of one per session.
  if (options_.memory_mode=="no-prepack") so.AddConfigEntry("session.disable_prepacking","1");
  if (options_.memory_mode=="shared-prepack") prepacked_=std::make_unique<Ort::PrepackedWeightsContainer>();
  auto decoder_session=[&](const fs::path& path){
    if (!options_.profile.empty()) so.EnableProfiling((options_.profile/path.stem()).c_str());
    return prepacked_?std::make_unique<Ort::Session>(env_,path.c_str(),so,*prepacked_):std::make_unique<Ort::Session>(env_,path.c_str(),so);
  };
  report("Loading decoder prefill"); init_=decoder_session(model/("decoder_init"+suffix));
  { Ort::AllocatorWithDefaultOptions allocator; auto name=init_->GetInputNameAllocated(0,allocator);
    prefill_ids_=std::string(name.get())=="input_ids";
    if (!prefill_ids_ && std::string(name.get())!="input_embeds") throw std::runtime_error("Unrecognized decoder prefill format"); }
  report("Loading cached decoder"); step_=decoder_session(model/("decoder_step"+suffix));
  report("Model ready");
}
std::vector<float> Engine::embedding(int token) const {
  if (token<0 || token>=vocab_) throw std::runtime_error("Token outside embedding vocabulary");
  std::vector<float> v(hidden_); size_t base=size_t(token)*hidden_;
  if (half_) { auto* p=static_cast<const Ort::Float16_t*>(embeddings_->data()); for (int i=0;i<hidden_;++i) v[i]=p[base+i].ToFloat(); }
  else memcpy(v.data(),static_cast<const float*>(embeddings_->data())+base,hidden_*4);
  return v;
}
Json Engine::inspect() const {
  Json j={{"runtime",Ort::GetVersionString()},{"provider",options_.directml?"DirectML (experimental)":"CPU"},{"threads",options_.threads},{"memory_mode",options_.memory_mode},{"assets_verified",options_.verify_assets},{"variant",options_.variant},
          {"model_directory",utf8(model_.filename().wstring())},{"model_hidden_size",hidden_},{"adapter_index",options_.directml?Json(options_.adapter):Json()},
          {"prefix_ids",prefix_},{"suffix_ids",suffix_},{"eos_ids",eos_},{"embedding_dtype",half_?"float16":"float32"},{"prefill_format",prefill_ids_?"input_ids":"input_embeds"}};
  if (fs::exists(model_/"manifest.json")) {
    auto manifest=Json::parse(read_text(model_/"manifest.json"));j["model_manifest_sha256"]=sha256_file(model_/"manifest.json");
    j["reference_model"]=manifest.at("reference_model");j["configuration"]=manifest.at("configuration");
  }
  Ort::AllocatorWithDefaultOptions allocator;
  for (auto [name,s]:std::vector<std::pair<std::string,Ort::Session*>>{{"encoder",encoder_.get()},{"prefill",init_.get()},{"step",step_.get()}}) {
    j[name]=Json::array();
    for (size_t i=0;i<s->GetInputCount();++i) { auto n=s->GetInputNameAllocated(i,allocator); auto type=s->GetInputTypeInfo(i); auto ti=type.GetTensorTypeAndShapeInfo(); j[name].push_back({{"name",n.get()},{"shape",ti.GetShape()},{"dtype",int(ti.GetElementType())}}); }
  }
  return j;
}
Json Engine::finish_profiling() {
  Json result=Json::array();
  if (options_.profile.empty()) return result;
  Ort::AllocatorWithDefaultOptions allocator;
  for (auto* session:{encoder_.get(),init_.get(),step_.get()}) {
    auto path=session->EndProfilingAllocated(allocator);result.push_back(path.get());
  }
  options_.profile.clear();return result;
}
Transcript Engine::transcribe(std::span<const float> audio, const fs::path& trace, const std::atomic<bool>* cancel, int max_tokens) {
  if (max_tokens<=0) max_tokens=options_.max_tokens;
  if (options_.slow_inference>0) std::this_thread::sleep_for(std::chrono::duration<double>(options_.slow_inference));  // deliberately slowed inference (tests)
  auto total=Clock::now(), stage=total;
  Transcript r;
  auto padded=model_samples(audio);
  auto m=mel_->compute(padded);
  r.detail["model_samples"]=padded.size();
  r.detail["preprocess_seconds"]=seconds(stage);
  if (!trace.empty()) { fs::create_directories(trace); write_floats(trace/"pcm.f32",padded); write_floats(trace/"mel.f32",m.values); }
  int64_t ms[]={1,128,m.frames};
  auto input=Ort::Value::CreateTensor<float>(cpu_,m.values.data(),m.values.size(),ms,3);
  const char* encin[]={"mel"}; const char* encout[]={"audio_features"};
  stage=Clock::now();
  auto encoded=encoder_->Run(Ort::RunOptions{nullptr},encin,&input,1,encout,1);
  r.detail["encoder_seconds"]=seconds(stage);
  auto ai=encoded[0].GetTensorTypeAndShapeInfo(); auto shape=ai.GetShape();
  if (shape.size()!=3 || shape[0]!=1 || shape[2]!=hidden_ || shape[1]!=MelExtractor::audio_tokens(m.frames)) throw std::runtime_error("Encoder feature length mismatch");
  if (!trace.empty()) write_floats(trace/"encoder.f32",{encoded[0].GetTensorData<float>(),ai.GetElementCount()});
  std::vector<int64_t> ids(prefix_.begin(),prefix_.end());
  int64_t offset=static_cast<int64_t>(ids.size());
  ids.insert(ids.end(),shape[1],config_.at("special_tokens").at("audio_pad_token_id").get<int>());
  ids.insert(ids.end(),suffix_.begin(),suffix_.end());
  std::vector<int64_t> positions(ids.size()); std::iota(positions.begin(),positions.end(),0);
  int64_t is[]={1,static_cast<int64_t>(ids.size())}, os[]={1};
  std::vector<float> prompt_embeds(ids.size()*hidden_);
  for (size_t i=0;i<ids.size();++i) { auto e=embedding(static_cast<int>(ids[i])); memcpy(prompt_embeds.data()+i*hidden_,e.data(),hidden_*sizeof(float)); }
  memcpy(prompt_embeds.data()+offset*hidden_,encoded[0].GetTensorData<float>(),ai.GetElementCount()*sizeof(float));
  if (!trace.empty()) write_floats(trace/"input_embeds.f32",prompt_embeds);
  std::vector<Ort::Value> inputs; std::vector<const char*> initnames;
  if (prefill_ids_) {
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(cpu_,ids.data(),ids.size(),is,2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(cpu_,positions.data(),positions.size(),is,2));
    inputs.push_back(std::move(encoded[0]));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(cpu_,&offset,1,os,1));
    initnames={"input_ids","position_ids","audio_features","audio_offset"};
  } else {
    int64_t es[]={1,static_cast<int64_t>(ids.size()),hidden_};
    inputs.push_back(Ort::Value::CreateTensor<float>(cpu_,prompt_embeds.data(),prompt_embeds.size(),es,3));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(cpu_,positions.data(),positions.size(),is,2));
    initnames={"input_embeds","position_ids"};
  }
  const char* outs[]={"logits","present_keys","present_values"};
  stage=Clock::now();
  auto state=init_->Run(Ort::RunOptions{nullptr},initnames.data(),inputs.data(),inputs.size(),outs,3);
  r.detail["prefill_seconds"]=seconds(stage);
  if (!trace.empty()) write_json(trace/"prompt.json",{{"ids",ids},{"positions",positions},{"audio_offset",offset},{"mel_frames",m.frames},{"audio_tokens",shape[1]}});
  double decode_time=0; Json cache_steps=Json::array();
  std::vector<int64_t> expected={config_.at("decoder").at("num_layers").get<int64_t>(),1,config_.at("decoder").at("num_key_value_heads").get<int64_t>(),0,config_.at("decoder").at("head_dim").get<int64_t>()};
  for (int t=0;t<max_tokens;++t) {
    auto ti=state[0].GetTensorTypeAndShapeInfo(); auto count=ti.GetElementCount();
    if (count<static_cast<size_t>(vocab_) || count%vocab_) throw std::runtime_error("Invalid logits shape");
    const float* logits=state[0].GetTensorData<float>()+count-vocab_;
    for (int i=0;i<vocab_;++i) if (!std::isfinite(logits[i])) throw std::runtime_error("Non-finite decoder logits");
    int next=static_cast<int>(std::max_element(logits,logits+vocab_)-logits);
    r.tokens.push_back(next);
    auto ks=state[1].GetTensorTypeAndShapeInfo().GetShape(); auto vs=state[2].GetTensorTypeAndShapeInfo().GetShape();
    cache_steps.push_back({{"step",t},{"keys",ks},{"values",vs},{"next_token",next}});
    expected[3]=static_cast<int64_t>(ids.size())+t;
    if (ks!=expected || vs!=expected) throw std::runtime_error("KV cache shape/growth mismatch");
    if (!trace.empty() && (t==0 || t==1 || t==7)) {
      write_floats(trace/("logits_"+std::to_string(t)+".f32"),{logits,static_cast<size_t>(vocab_)});
      for (int q=1;q<=2;++q) { auto ct=state[q].GetTensorTypeAndShapeInfo(); write_floats(trace/((q==1?"keys_":"values_")+std::to_string(t)+".f32"),{state[q].GetTensorData<float>(),ct.GetElementCount()}); }
    }
    if (std::find(eos_.begin(),eos_.end(),next)!=eos_.end()) { r.completion="eos"; break; }
    if (t+1==max_tokens) { r.completion="token_limit"; break; }
    if (cancel && cancel->load(std::memory_order_relaxed)) { r.completion="cancelled"; break; }
    auto embed=embedding(next);
    int64_t es[]={1,1,hidden_}, ps[]={1,1}; int64_t pos=static_cast<int64_t>(ids.size())+t;
    std::vector<Ort::Value> sin;
    sin.push_back(Ort::Value::CreateTensor<float>(cpu_,embed.data(),embed.size(),es,3));
    sin.push_back(Ort::Value::CreateTensor<int64_t>(cpu_,&pos,1,ps,2));
    sin.push_back(std::move(state[1])); sin.push_back(std::move(state[2]));
    const char* names[]={"input_embeds","position_ids","past_keys","past_values"};
    stage=Clock::now();
    state=step_->Run(Ort::RunOptions{nullptr},names,sin.data(),sin.size(),outs,3);
    decode_time+=seconds(stage);
  }
  r.raw=tokenizer_->Decode(r.tokens);
  auto start=r.raw.find("<asr_text>");
  if (start!=std::string::npos) r.text=r.raw.substr(start+10);
  else if (r.completion=="cancelled" || r.completion=="token_limit" || r.raw.find("<|im_end|>")==0 || r.raw.find("<|endoftext|>")==0) r.text="";
  else throw std::runtime_error("ASR output missing <asr_text> delimiter");
  for (auto marker:{"<|im_end|>","<|endoftext|>"}) if (auto p=r.text.find(marker); p!=std::string::npos) r.text.erase(p);
  r.detail["decode_seconds"]=decode_time; r.detail["total_seconds"]=seconds(total);
  r.detail["audio_seconds"]=audio.size()/16000.0; r.detail["rtf"]=seconds(total)/(audio.size()/16000.0);
  r.detail["tokens"]=r.tokens; r.detail["raw_output"]=r.raw; r.detail["text"]=r.text; r.detail["completion"]=r.completion;
  r.detail["cache_steps"]=cache_steps; r.detail["memory"]=memory_usage();
  if (!trace.empty()) write_json(trace/"result.json",r.detail);
  return r;
}
}
